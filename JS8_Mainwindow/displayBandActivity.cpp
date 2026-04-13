

/** \file
 * @brief member function of the UI_Constructor class
 *  displays the current band activity in the left pane of the UI
 */

#include "JS8_UI/mainwindow.h"

void UI_Constructor::displayBandActivity() {
    auto now = DriftingDateTime::currentDateTimeUtc();

    // Reset the header label text to accommodate minimal label setting
    int cols = ui->tableWidgetRXAll->columnCount();
    for (int c = 0; c < cols; ++c) {
        ui->tableWidgetRXAll->horizontalHeaderItem(c)->setText(
            columnLabel(m_origRxHeaderLabelMap[c]));
    }

    ui->tableWidgetRXAll->setFont(m_config.table_font());

    // Selected Offset
    int selectedOffset = -1;
    auto selectedItems = ui->tableWidgetRXAll->selectedItems();
    if (!selectedItems.isEmpty()) {
        selectedOffset = selectedItems.first()->data(Qt::UserRole).toInt();
    }

    ui->tableWidgetRXAll->setUpdatesEnabled(false);
    {
        // Scroll Position
        auto const currentScrollPos =
            ui->tableWidgetRXAll->verticalScrollBar()->value();

        // Clear the table
        ui->tableWidgetRXAll->setRowCount(0);

        // Sort!
        auto const sort = getSortByReverse("bandActivity", "offset");
        auto keys = m_bandActivity.keys();

        // Base comparison, called by the detail comparisons. We may not need
        // to proceed to the detail comparison at all here, if at least one of
        // the lists is empty. If both are non-empty, delegate to the detail
        // comparison, providing it with the last list elements.

        auto const compare = [this](int const lhsKey, int const rhsKey,
                                    auto &&detail) {
            auto const &lhs = m_bandActivity[lhsKey];
            auto const &rhs = m_bandActivity[rhsKey];

            if (lhs.isEmpty())
                return false;
            if (rhs.isEmpty())
                return true;

            return detail(lhs.last(), rhs.last());
        };

        // Time stamp comparison, easy stuff, just a total ordering on the
        // UTC time stamp field.

        auto const compareTimestamp = [compare](int const lhsKey,
                                                int const rhsKey) {
            return compare(lhsKey, rhsKey, [](auto &&lhs, auto &&rhs) {
                return lhs.utcTimestamp < rhs.utcTimestamp;
            });
        };

        // SNR comparison;  we always want insane SNR values to be at the end
        // of the list and the list is going to be reversed if reverse is set,
        // so we want to set things up so that insane elements are either all
        // at the beginning in the case of a reverse, or all at the end in the
        // standard case. Reverse takes care of itself; we just need to sort
        // out standard.

        auto const compareSNR = [compare, reverse = sort.reverse](
                                    int const lhsKey, int const rhsKey) {
            return compare(lhsKey, rhsKey, [reverse](auto &&lhs, auto &&rhs) {
                auto lhsSNR = lhs.snr;
                auto rhsSNR = rhs.snr;

                if (!reverse) {
                    if (lhsSNR < -60 || lhsSNR > 60)
                        lhsSNR = -lhsSNR;
                    if (rhsSNR < -60 || rhsSNR > 60)
                        rhsSNR = -rhsSNR;
                }

                return lhsSNR < rhsSNR;
            });
        };

        // Submode comparison; slow mode isn't at the start of the enumeration;
        // it's in the middle of it. All the other modes are in the expected
        // order.

        auto const compareSubmode = [compare](int const lhsKey,
                                              int const rhsKey) {
            return compare(lhsKey, rhsKey, [](auto &&lhs, auto &&rhs) {
                auto lhsSubmode = lhs.submode;
                auto rhsSubmode = rhs.submode;

                if (lhsSubmode == Varicode::JS8CallSlow)
                    lhsSubmode = -lhsSubmode;
                if (rhsSubmode == Varicode::JS8CallSlow)
                    rhsSubmode = -rhsSubmode;

                return lhsSubmode < rhsSubmode;
            });
        };

        // Always perform an initial sort by offset.

        std::stable_sort(keys.begin(), keys.end());

        // If something other than offset was requested as the sort by, perform
        // an additional stable sort by the field requested.

        if (sort.by == "timestamp")
            std::stable_sort(keys.begin(), keys.end(), compareTimestamp);
        else if (sort.by == "snr")
            std::stable_sort(keys.begin(), keys.end(), compareSNR);
        else if (sort.by == "submode")
            std::stable_sort(keys.begin(), keys.end(), compareSubmode);

        // The sort comparators leave things in forward order. If a reverse sort
        // was requested, reverse the keys.

        if (sort.reverse)
            std::reverse(keys.begin(), keys.end());

        // Build the table
        foreach (int offset, keys) {
            bool isOffsetSelected = (offset == selectedOffset);

            QList<ActivityDetail> items = m_bandActivity[offset];
            if (items.length() > 0) {
                QDateTime timestamp;
                QStringList text;
                QString age;
                int snr = 0;
                bool snrSuspect = false;
                float tdrift = 0;
                int submode = -1;

                int activityAging = m_config.activity_aging();

                // hide items that shouldn't appear

                for (int i = 0; i < items.length(); i++) {
                    auto item = items[i];

                    bool shouldDisplay = true;

                    // hide aged items
                    if (!isOffsetSelected && activityAging &&
                        item.utcTimestamp.secsTo(now) / 60 >= activityAging) {
                        shouldDisplay = false;
                    }

                    // hide heartbeat items
                    if (!ui->actionShow_Band_Heartbeats_and_ACKs->isChecked()) {
                        // hide heartbeats and acks if we have heartbeating
                        // hidden
                        if (item.text.contains(" @HB ") ||
                            item.text.contains(" HEARTBEAT ")) {
                            shouldDisplay = false;

                            // hide the previous item if this it shouldn't be
                            // displayed either...
                            if (i > 0 && items[i - 1].shouldDisplay &&
                                items[i - 1].text.endsWith(": ")) {
                                items[i - 1].shouldDisplay = false;
                            }
                        }

                        // if our previous item should not be displayed (or this
                        // is the first frame) and we have a MSG ID, then don't
                        // display it either.
                        if ((i == 0 ||
                             (i > 0 && !items[i - 1].shouldDisplay)) &&
                            (item.text.contains(" MSG ID "))) {
                            shouldDisplay = false;
                        }
                    }

                    // hide empty items
                    if (item.text.isEmpty()) {
                        shouldDisplay = false;
                    }

                    // set the visibility of the item
                    items[i].shouldDisplay = shouldDisplay;
                }

                // show the items that should appear, grouped by callsign
                // Each group: {callsign, accumulated text}
                struct CallGroup { QString call; QString text; };
                QList<CallGroup> callGroups;
                QString currentCall;

                foreach (ActivityDetail item, items) {
                    if (!item.shouldDisplay) {
                        continue;
                    }

                    if (item.isLowConfidence) {
                        item.text = QString("[%1]").arg(item.text);
                    }

                    if ((item.bits & Varicode::JS8CallLast) ==
                        Varicode::JS8CallLast) {
                        item.text = QString("%1 %2 ")
                                        .arg(Varicode::rstrip(item.text))
                                        .arg(m_config.eot());
                    }

                    // Extract "from" callsign from "CALL: text" pattern
                    // Format is always CALLSIGN: @GROUP/message — the callsign
                    // is the sender, @GROUP is the destination.
                    QString frameCall;
                    int colonPos = item.text.indexOf(": ");
                    if (colonPos > 0 && colonPos <= 15) {
                        QString candidate = item.text.left(colonPos).trimmed();
                        // Valid callsign: 3-15 chars, has letter and digit
                        if (candidate.length() >= 3 && candidate.length() <= 15
                            && candidate.contains(QRegularExpression("[A-Z]"))
                            && candidate.contains(QRegularExpression("[0-9]"))) {
                            frameCall = candidate;
                        }
                    }

                    // Assign frame to callsign group — consolidate by callsign
                    int maxGroups = m_config.message_subdivisions();
                    if (!frameCall.isEmpty()) {
                        // Find existing group for this callsign
                        int existingIdx = -1;
                        for (int g = 0; g < callGroups.size(); g++) {
                            if (callGroups[g].call == frameCall) {
                                existingIdx = g;
                                break;
                            }
                        }
                        if (existingIdx >= 0) {
                            // Append to existing group for this callsign
                            callGroups[existingIdx].text += item.text;
                        } else if (callGroups.size() < maxGroups) {
                            // New callsign — create new group
                            callGroups.append({frameCall, item.text});
                        } else {
                            // No room — drop oldest subdivision to make
                            // room for the most recent callsign.
                            callGroups.removeFirst();
                            callGroups.append({frameCall, item.text});
                        }
                        currentCall = frameCall;
                    } else if (frameCall.isEmpty()) {
                        // Anonymous frame — append to current callsign's group
                        // as continuation. If no group exists yet, skip it
                        // (orphan fragment, visible via tooltip on other columns).
                        if (!callGroups.isEmpty()) {
                            callGroups.last().text += item.text;
                        }
                    } else {
                        // Continuation frame (same callsign) — append to current group
                        callGroups.last().text += item.text;
                    }

                    text.append(item.text);
                    snr = item.snr;
                    snrSuspect = item.snrSuspect;
                    age = since(item.utcTimestamp);
                    timestamp = item.utcTimestamp;
                    tdrift = item.tdrift;
                    submode = item.submode;
                }

                auto joined = Varicode::rstrip(text.join(""));
                if (joined.isEmpty()) {
                    continue;
                }

                ui->tableWidgetRXAll->insertRow(
                    ui->tableWidgetRXAll->rowCount());
                int row = ui->tableWidgetRXAll->rowCount() - 1;
                int col = 0;

                auto offsetItem = new QTableWidgetItem(
                    QString(columnLabel("%1 Hz")).arg(offset));
                offsetItem->setData(Qt::UserRole, QVariant(offset));
                offsetItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                ui->tableWidgetRXAll->setItem(row, col++, offsetItem);

                auto tdriftItem = new QTableWidgetItem(
                    QString(columnLabel("%1 ms")).arg((int)(1000 * tdrift)));
                tdriftItem->setData(Qt::UserRole, QVariant(tdrift));
                tdriftItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                ui->tableWidgetRXAll->setItem(row, col++, tdriftItem);

                auto ageItem = new QTableWidgetItem(age);
                ageItem->setTextAlignment(Qt::AlignCenter);
                ageItem->setToolTip(timestamp.toString());
                ui->tableWidgetRXAll->setItem(row, col++, ageItem);

                auto snrText = snrSuspect ? QString() : Varicode::formatSNR(snr);
                auto snrItem = new QTableWidgetItem(
                    snrText.isEmpty()
                        ? ""
                        : QString(columnLabel("%1 dB")).arg(snrText));
                snrItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                ui->tableWidgetRXAll->setItem(row, col++, snrItem);

                auto name = JS8::Submode::name(submode);
                auto displayChar = (submode == Varicode::JS8CallFT2)
                    ? QString::fromUtf8("\u26A1")
                    : name.left(1).replace("H", "N");
                auto submodeItem = new QTableWidgetItem(displayChar);
                submodeItem->setToolTip(name);
                submodeItem->setData(Qt::UserRole, QVariant(submode));
                submodeItem->setTextAlignment(Qt::AlignCenter);
                ui->tableWidgetRXAll->setItem(row, col++, submodeItem);

                // Store callsign groups for sub-divided rendering
                QVariantList groupData;
                for (const auto &g : callGroups) {
                    QVariantMap m;
                    m["call"] = g.call;
                    m["text"] = Varicode::rstrip(g.text);
                    groupData.append(m);
                }

                auto textItem = new QTableWidgetItem(joined);
                textItem->setData(Qt::UserRole, groupData);
                textItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);

                ui->tableWidgetRXAll->setItem(row, col++, textItem);

                // Show full line contents as tooltip on Offset, Time Delta,
                // and SNR columns (not Age or Speed — they have their own tips)
                // Bold callsign: patterns for readability
                {
                    auto fullTip = joined.toHtmlEscaped();
                    static const QRegularExpression callRe(R"((\b(?=[A-Z0-9/]*[0-9])[A-Z0-9/]{3,15}:)(?=\s))");
                    fullTip.replace(callRe, "<br/><b>\\1</b>");
                    if (fullTip.startsWith("<br/>"))
                        fullTip = fullTip.mid(5);
                    fullTip = QString("<div style='white-space:nowrap;'>%1</div>").arg(fullTip);
                    offsetItem->setToolTip(fullTip);
                    tdriftItem->setToolTip(fullTip);
                    snrItem->setToolTip(fullTip);
                }

                if (isOffsetSelected) {
                    for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                         i++) {
                        ui->tableWidgetRXAll->item(row, i)->setSelected(true);
                    }
                }

                bool isDirectedAllCall = false;
                if ((isDirectedOffset(offset, &isDirectedAllCall) &&
                     !isDirectedAllCall) ||
                    isMyCallIncluded(text.last())) {
                    for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                         i++) {
                        ui->tableWidgetRXAll->item(row, i)->setBackground(
                            QBrush(m_config.color_MyCall()));
                    }
                }

                if (!text.isEmpty()) {
                    auto const list = joined.split(QRegularExpression("[:> ]"),
                                                   Qt::SkipEmptyParts);
                    QSet<QString> words(list.begin(), list.end());

                    if (words.contains("CQ")) {
                        for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                             i++) {
                            ui->tableWidgetRXAll->item(row, i)->setBackground(
                                QBrush(m_config.color_CQ()));
                        }
                    }

                    auto matchingSecondaryWords =
                        m_config.secondary_highlight_words() & words;
                    if (!matchingSecondaryWords.isEmpty()) {
                        for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                             i++) {
                            ui->tableWidgetRXAll->item(row, i)->setBackground(
                                QBrush(m_config.color_secondary_highlight()));
                        }
                    }

                    auto matchingPrimaryWords =
                        m_config.primary_highlight_words() & words;
                    if (!matchingPrimaryWords.isEmpty()) {
                        for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                             i++) {
                            ui->tableWidgetRXAll->item(row, i)->setBackground(
                                QBrush(m_config.color_primary_highlight()));
                        }
                    }
                }
            }
        }

        // Set table color
        auto style = QString(
            "QTableWidget { background:%1; selection-background-color:%2; "
            "alternate-background-color:%1; color:%3; } "
            "QTableWidget::item:selected { background-color: %2; color: %3; }");
        style = style.arg(m_config.color_table_background().name());
        style = style.arg(m_config.color_table_highlight().name());
        style = style.arg(m_config.color_table_foreground().name());
        ui->tableWidgetRXAll->setStyleSheet(style);

        // Set the table palette for inactive selected row
        auto p = ui->tableWidgetRXAll->palette();

        p.setColor(QPalette::Highlight, m_config.color_table_highlight());
        p.setColor(QPalette::HighlightedText,
                   m_config.color_table_foreground());
        p.setColor(QPalette::Inactive, QPalette::Highlight,
                   p.color(QPalette::Active, QPalette::Highlight));
        ui->tableWidgetRXAll->setPalette(p);

        // Set item fonts
        for (int row = 0; row < ui->tableWidgetRXAll->rowCount(); row++) {
            for (int col = 0; col < ui->tableWidgetRXAll->columnCount();
                 col++) {
                auto item = ui->tableWidgetRXAll->item(row, col);
                if (item) {
                    item->setFont(m_config.table_font());
                }
            }
        }

        // Column labels
        ui->tableWidgetRXAll->horizontalHeader()->setVisible(
            showColumn("band", "labels"));

        // Hide columns
        ui->tableWidgetRXAll->setColumnHidden(0, !showColumn("band", "offset"));
        ui->tableWidgetRXAll->setColumnHidden(
            1, !showColumn("band", "tdrift"));
        ui->tableWidgetRXAll->setColumnHidden(2,
                                              !showColumn("band", "timestamp"));
        ui->tableWidgetRXAll->setColumnHidden(3, !showColumn("band", "snr"));
        ui->tableWidgetRXAll->setColumnHidden(
            4, !showColumn("band", "submode", false));

        // Resize the table columns
        ui->tableWidgetRXAll->resizeColumnToContents(0);
        ui->tableWidgetRXAll->resizeColumnToContents(1);
        ui->tableWidgetRXAll->resizeColumnToContents(2);
        ui->tableWidgetRXAll->resizeColumnToContents(3);
        ui->tableWidgetRXAll->resizeColumnToContents(4);

        // Reset the scroll position
        ui->tableWidgetRXAll->verticalScrollBar()->setValue(currentScrollPos);
    }
    ui->tableWidgetRXAll->setUpdatesEnabled(true);
}
