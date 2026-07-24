/** \file
 * @brief UI_Constructor::checkVersion — startup + Help-menu update check.
 *
 * Rewritten 2026-07-23 (was legacy upstream JS8Call code that had NEVER
 * worked in this fork: it fetched a `version.txt` release asset that is
 * not published, so the request 404'd and the function returned silently
 * — the dialog could never appear). Now:
 *
 *   - Reads the latest version from the GitHub RELEASES API `tag_name`,
 *     which the release creates automatically — no hand-published asset
 *     to forget (that omission is exactly what hid the old breakage).
 *   - Compares at BUILD granularity: version() is "4.1.0.<build>" and
 *     QVersionNumber compares all four components.
 *   - Notifies ONCE per new version (LastUpdateNotifiedVersion setting);
 *     the Help-menu manual check (alertOnUpToDate) is unconditional.
 *   - Picks the message + link by DELIVERY channel, detected at runtime
 *     (the Store build is an MSIX repackage of the SAME .exe, so a
 *     build-time flag cannot tell them apart — only package identity
 *     can): MSIX -> Microsoft Store page ("Update now."), otherwise the
 *     GitHub release page ("More information here.").
 *   - Logs failures VISIBLY (qWarning). The old code buried them in a
 *     qCDebug, which is why the breakage went unnoticed.
 */

#include "JS8_UI/mainwindow.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QVersionNumber>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// True when this process is running from an MSIX/APPX package (the
// Microsoft Store delivery). Resolved dynamically from kernel32 so it
// needs no import lib or appmodel.h on the MinGW toolchain, and is a
// no-op everywhere but Windows. GetCurrentPackageFullName returns
// APPMODEL_ERROR_NO_PACKAGE (15700) when unpackaged; any other result
// (typically ERROR_INSUFFICIENT_BUFFER, because a package name exists)
// means packaged.
bool runningAsAppx() {
#ifdef Q_OS_WIN
    if (auto *k32 = GetModuleHandleW(L"kernel32.dll")) {
        using Fn = LONG(WINAPI *)(UINT32 *, PWSTR);
        if (auto fn = reinterpret_cast<Fn>(
                GetProcAddress(k32, "GetCurrentPackageFullName"))) {
            UINT32 len = 0;
            LONG const rc = fn(&len, nullptr);
            constexpr LONG APPMODEL_ERROR_NO_PACKAGE_ = 15700;
            return rc != APPMODEL_ERROR_NO_PACKAGE_;
        }
    }
    return false;  // pre-Win8 GetProcAddress miss => treat as unpackaged
#else
    return false;
#endif
}

// Microsoft Store product page for Subspace Edition (product id
// 9nhk74bnkxkc). Opening it lands the user on the Store's Update button.
// Store apps auto-update ONLY while signed into the Store — which most
// users are not — so this nudge is the real update path for them, not a
// duplicate of a reliable background updater.
constexpr char kStoreUrl[] = "https://apps.microsoft.com/detail/9nhk74bnkxkc";
constexpr char kReleasesUrl[] =
    "https://github.com/avantol/Subspace-Edition/releases";

// [2026-07-24 appxStoreCheck] When running as the MSIX/Store build, the
// update check must consult the STORE's version, not GitHub, and must
// never mention GitHub — the Store is that channel's sole authority.
// This is the same public REST source `winget --source msstore` uses;
// GET .../packageManifests/<ProductId> returns
//   { "Data": { "Versions": [ { "PackageVersion": "4.1.0.<build>.0" }, ... ] } }
// We take the max PackageVersion. Best-effort: if this endpoint ever
// changes shape or fails, the appx path degrades to "open the Store"
// (see the failure handling) — it NEVER falls back to GitHub.
// NOTE: for the comparison to be correct the MSIX package version must
// track version() = "4.1.0.<build>" (MSIX pads a 4th revision digit,
// e.g. "4.1.0.<build>.0" — QVersionNumber treats the trailing 0 as
// equal, so that is fine).
constexpr char kStoreProductId[] = "9NHK74BNKXKC";
constexpr char kStoreManifestUrl[] =
    "https://storeedgefd.dsx.mp.microsoft.com/v9.0/packageManifests/"
    "9NHK74BNKXKC?market=US&locale=en-US";

}  // namespace

// [2026-07-24 appxStoreCheck] Parse the newest version out of the Store
// (winget msstore) packageManifests JSON: Data.Versions[].PackageVersion,
// take the max. Returns empty on any shape mismatch (=> graceful Store
// fallback, never GitHub).
static QString parseStoreLatest(QByteArray const &body) {
    auto const versions = QJsonDocument::fromJson(body)
                              .object()
                              .value(QStringLiteral("Data"))
                              .toObject()
                              .value(QStringLiteral("Versions"))
                              .toArray();
    QVersionNumber best;
    QString bestStr;
    for (auto const &v : versions) {
        QString const s = v.toObject()
                              .value(QStringLiteral("PackageVersion"))
                              .toString()
                              .trimmed();
        auto const vn = QVersionNumber::fromString(s);
        if (!vn.isNull() && best < vn) {
            best = vn;
            bestStr = s;
        }
    }
    return bestStr;
}

void UI_Constructor::checkVersion(bool const alertOnUpToDate) {
    // [2026-07-24 appxStoreCheck] Channel decides EVERYTHING: which
    // version source we query and which store/site (if any) we point at.
    // The MSIX/Store build checks the STORE version and never mentions
    // GitHub; the plain build checks GitHub. Resolved once here.
    bool const appx = runningAsAppx();

    auto *m = new QNetworkAccessManager(this);
    connect(
        m, &QNetworkAccessManager::finished, this,
        [this, alertOnUpToDate, appx](QNetworkReply *reply) {
            reply->manager()->deleteLater();
            reply->deleteLater();

            // Parse the latest version per channel; htmlUrl is GitHub-only.
            QString latestStr;
            QString htmlUrl;
            if (!reply->error()) {
                if (appx) {
                    latestStr = parseStoreLatest(reply->readAll());
                } else {
                    auto const obj =
                        QJsonDocument::fromJson(reply->readAll()).object();
                    QString tag =
                        obj.value(QStringLiteral("tag_name")).toString().trimmed();
                    bool const isDraft = obj.value(QStringLiteral("draft")).toBool();
                    bool const isPre =
                        obj.value(QStringLiteral("prerelease")).toBool();
                    htmlUrl = obj.value(QStringLiteral("html_url")).toString();
                    if (!tag.isEmpty() && !isDraft && !isPre) {
                        if (tag.startsWith('v') || tag.startsWith('V'))
                            tag = tag.mid(1);
                        latestStr = tag;
                    }
                }
            }

            // Failure = network error OR unparseable/no usable version.
            if (latestStr.isEmpty()) {
                // VISIBLE, not qCDebug — a silent failure here is what hid
                // the old breakage for the life of the fork.
                qWarning() << "[UPDATE-CHECK]"
                           << (appx ? "store" : "github")
                           << "check failed:" << reply->errorString() << "http="
                           << reply
                                  ->attribute(
                                      QNetworkRequest::HttpStatusCodeAttribute)
                                  .toInt();
                if (alertOnUpToDate) {
                    // appx: offer the Store (never GitHub); plain: generic.
                    QString const msg =
                        appx ? tr("Could not check the Microsoft Store right "
                                  "now. <a href=\"%1\">Open the Microsoft "
                                  "Store</a> to check for updates.")
                                   .arg(QString::fromLatin1(kStoreUrl))
                             : tr("Could not check for a new version right "
                                  "now. Please try again later.");
                    (new SelfDestructMessageBox(
                         30, tr("Update Check Failed"), msg,
                         QMessageBox::Warning, QMessageBox::Ok,
                         QMessageBox::Ok, false, this))
                        ->show();
                }
                return;
            }

            auto const currentVersion = QVersionNumber::fromString(version());
            auto const latestVersion = QVersionNumber::fromString(latestStr);

            qWarning() << "[UPDATE-CHECK]" << (appx ? "store" : "github")
                       << "current=" << currentVersion
                       << "latest=" << latestVersion
                       << "newer=" << (currentVersion < latestVersion);

            if (currentVersion < latestVersion) {
                QString const normStr = latestVersion.toString();

                // Once per new version — but only for the automatic
                // (startup) check; the Help-menu check always reports.
                if (!alertOnUpToDate) {
                    QSettings settings;
                    if (settings
                            .value(QStringLiteral(
                                "Common/LastUpdateNotifiedVersion"))
                            .toString() == normStr) {
                        qWarning() << "[UPDATE-CHECK] already notified for"
                                   << normStr << "— suppressing";
                        return;
                    }
                    settings.setValue(
                        QStringLiteral("Common/LastUpdateNotifiedVersion"),
                        normStr);
                }

                QString link, linkText;
                if (appx) {
                    // STORE ONLY — no GitHub link ever on this channel.
                    link = QString::fromLatin1(kStoreUrl);
                    linkText = tr("Update now.");
                } else {
                    link = htmlUrl.isEmpty() ? QString::fromLatin1(kReleasesUrl)
                                             : htmlUrl;
                    linkText = tr("More information here.");
                }

                (new SelfDestructMessageBox(
                     60, tr("New Version Available"),
                     tr("A new version of Subspace Edition is available. "
                        "<a href=\"%1\">%2</a>")
                         .arg(link, linkText),
                     QMessageBox::Information, QMessageBox::Ok,
                     QMessageBox::Ok, false, this))
                    ->show();
            } else if (alertOnUpToDate) {
                (new SelfDestructMessageBox(
                     60, tr("No Updates Available"),
                     tr("Your version (%1) of Subspace Edition is "
                        "up-to-date.")
                         .arg(version()),
                     QMessageBox::Information, QMessageBox::Ok,
                     QMessageBox::Ok, false, this))
                    ->show();
            }
        });

    QNetworkRequest r;
    if (appx) {
        qWarning() << "[UPDATE-CHECK] querying Microsoft Store manifest for"
                   << kStoreProductId;
        r.setUrl(QUrl(QString::fromLatin1(kStoreManifestUrl)));
    } else {
        qWarning() << "[UPDATE-CHECK] querying GitHub releases API";
        r.setUrl(QUrl(QStringLiteral("https://api.github.com/repos/avantol/"
                                     "Subspace-Edition/releases/latest")));
        // GitHub rejects requests with NO Accept header on some paths.
        r.setRawHeader("Accept", "application/vnd.github+json");
    }
    // A User-Agent is required by GitHub (403 without it) and is polite to
    // the Store endpoint — set it for both. Its absence is exactly the
    // silent-failure class this code exists to avoid.
    r.setHeader(QNetworkRequest::UserAgentHeader,
                QStringLiteral("Subspace-Edition/%1").arg(version()));
    m->get(r);
}
