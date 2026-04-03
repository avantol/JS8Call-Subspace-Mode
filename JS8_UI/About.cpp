/**
 * @file About.cpp
 * @brief implementation of the About dialog for the UI
 */

#include "About.h"
#include "JS8_Main/revision_utils.h"
#include "ui_About.h"

#include <QString>

CAboutDlg::CAboutDlg(QWidget *parent) : QDialog(parent), ui(new Ui::CAboutDlg) {
    ui->setupUi(this);
    setWindowTitle("About JS8Call Subspace Edition");
    ui->labelTxt->setText(QString{
        "<h2><a href=\"https://github.com/avantol/JS8Call-Subspace-Mode\">%1</a></h2>"
        "<h3>The Subspace Edition by <a href=\"https://www.qrz.com/db/WM8Q\">WM8Q</a> "
        "adds FT2-based async decoding<br />with time-independent sync, "
        "achieving -15 dB SNR sensitivity<br />at 5 characters/second.</h3>"
        "<h4>JS8Call Subspace Edition was derived from 'JS8Call-Improved'</h4>"
        "<p align='left'>JS8Call-Improved was developed by the team that "
        "continued development of the original "
        "js8call starting in late 2024.<br />This team includes: <br />Chris "
        "AC9KH, Allan W6BAZ, "
        "Wyatt KJ4CTD, Joe K0OG, Andreas DJ3EI, Rob K4RWR, Jordan KN4CRD.<br "
        "/><br />"
        "Legal Notice:<br />"
        "JS8Call 2.4 and later uses libraries from the FFmpeg project under "
        "the LGPLv2.1</p>"
        "<h4>What is JS8Call?</h4>"
        "<p align='left'>JS8Call is a derivative of the WSJT-X application, "
        "restructured and redesigned for message passing. <br/>"
        "It is not supported by nor endorsed by the WSJT-X "
        "development group. <br/>JS8Call is "
        "licensed under and in accordance with the terms "
        "of the <a href=\"https://www.gnu.org/licenses/gpl-3.0.txt\">GPLv3 "
        "license</a>.<br/><br/>"

        "JS8Call is heavily inspired by WSJT-X, Fldigi and FSQCall and would "
        "not exist without the hard work<br />"
        "and dedication of the many developers in the amateur radio "
        "community.<br /><br />"
        "JS8Call stands on the shoulder of giants...the takeoff angle is "
        "better up there.<br /><br />"
        "A special thanks goes out to:<br/><br/><strong>"
        "KC9QNE, "
        "KI6SSI, "
        "K0OG, "
        "LB9YH, "
        "M0IAX, "
        "N0JDS, "
        "OH8STN, "
        "VA3OSO, "
        "VK1MIC, "
        "W0FW, "
        "W6BAZ,</strong><br/><br/>"
        "and the many other amateur radio operators who have helped bring "
        "JS8Call into the world.</p>"}
                              .arg(program_title()));
}

CAboutDlg::~CAboutDlg() {}
