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

}  // namespace

void UI_Constructor::checkVersion(bool const alertOnUpToDate) {
    auto *m = new QNetworkAccessManager(this);
    connect(
        m, &QNetworkAccessManager::finished, this,
        [this, alertOnUpToDate](QNetworkReply *reply) {
            reply->manager()->deleteLater();
            reply->deleteLater();

            if (reply->error()) {
                // VISIBLE, not qCDebug — a silent failure here is what hid
                // the old breakage for the life of the fork.
                qWarning()
                    << "[UPDATE-CHECK] failed:" << reply->errorString()
                    << "http="
                    << reply
                           ->attribute(
                               QNetworkRequest::HttpStatusCodeAttribute)
                           .toInt();
                if (alertOnUpToDate) {
                    (new SelfDestructMessageBox(
                         30, tr("Update Check Failed"),
                         tr("Could not check for a new version right now. "
                            "Please try again later."),
                         QMessageBox::Warning, QMessageBox::Ok,
                         QMessageBox::Ok, false, this))
                        ->show();
                }
                return;
            }

            auto const obj = QJsonDocument::fromJson(reply->readAll()).object();
            QString tag = obj.value("tag_name").toString().trimmed();
            bool const isDraft = obj.value("draft").toBool();
            bool const isPre = obj.value("prerelease").toBool();
            QString const htmlUrl = obj.value("html_url").toString();

            if (tag.isEmpty() || isDraft || isPre) {
                qWarning() << "[UPDATE-CHECK] no usable stable release:"
                           << "tag=" << tag << "draft=" << isDraft
                           << "prerelease=" << isPre;
                if (alertOnUpToDate) {
                    (new SelfDestructMessageBox(
                         30, tr("Update Check Failed"),
                         tr("Could not read the latest version. Please try "
                            "again later."),
                         QMessageBox::Warning, QMessageBox::Ok,
                         QMessageBox::Ok, false, this))
                        ->show();
                }
                return;
            }

            // tag is "v4.1.0.<build>"; QVersionNumber wants digits first.
            if (tag.startsWith('v') || tag.startsWith('V')) {
                tag = tag.mid(1);
            }
            auto const currentVersion = QVersionNumber::fromString(version());
            auto const latestVersion = QVersionNumber::fromString(tag);

            qWarning() << "[UPDATE-CHECK] current=" << currentVersion
                       << "latest=" << latestVersion
                       << "newer=" << (currentVersion < latestVersion);

            if (currentVersion < latestVersion) {
                QString const latestStr = latestVersion.toString();

                // Once per new version — but only for the automatic
                // (startup) check; the Help-menu check always reports.
                if (!alertOnUpToDate) {
                    QSettings settings;
                    if (settings
                            .value(QStringLiteral(
                                "Common/LastUpdateNotifiedVersion"))
                            .toString() == latestStr) {
                        qWarning() << "[UPDATE-CHECK] already notified for"
                                   << latestStr << "— suppressing";
                        return;
                    }
                    settings.setValue(
                        QStringLiteral("Common/LastUpdateNotifiedVersion"),
                        latestStr);
                }

                QString link, linkText;
                if (runningAsAppx()) {
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

    qWarning() << "[UPDATE-CHECK] querying GitHub releases API"
               << "(appx=" << runningAsAppx() << ")";
    QNetworkRequest r(
        QUrl("https://api.github.com/repos/avantol/Subspace-Edition/"
             "releases/latest"));
    // GitHub's API rejects requests with NO User-Agent (HTTP 403) — the
    // exact class of silent failure this rewrite is meant to kill.
    r.setHeader(QNetworkRequest::UserAgentHeader,
                QStringLiteral("Subspace-Edition/%1").arg(version()));
    r.setRawHeader("Accept", "application/vnd.github+json");
    m->get(r);
}
