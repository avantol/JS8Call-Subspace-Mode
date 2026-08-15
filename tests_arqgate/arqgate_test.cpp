#include "JS8_Main/ChunkedArq.h"
#include <cstdio>
using ChunkedArq::TextClass;
using ChunkedArq::classifyOutgoingText;

struct Case { char const *text; TextClass want; char const *why; };
static Case const cases[] = {
    // ---- Andy's reported specimens (2026-07-17) ----
    {"@ALLCALL QUERY CALL WM8Q?", TextClass::DirectedCommand, "specimen"},
    {"wm8q/p QUERY CALL wm8q?",   TextClass::DirectedCommand, "specimen"},
    {"@ALLCALL QUERY MSGS",       TextClass::DirectedCommand, "specimen"},
    {"@ALLCALL QUERY MSG 1",      TextClass::DirectedCommand, "specimen"},
    {"@ALLCALL QUERY ARQ?",       TextClass::DirectedCommand, "specimen"},
    {"wm8q: k9avt QUERY ARQ?",    TextClass::DirectedCommand, "specimen paste-back"},
    {"k9avt QUERY ARQ?",          TextClass::DirectedCommand, "specimen"},
    {"QUERY ARQ?",                TextClass::DirectedCommand, "specimen bare"},
    {"K9AVT AVHAIL?",             TextClass::DirectedCommand, "AVHAIL directed"},
    {"@ALLCALL AVHAIL?",          TextClass::DirectedCommand, "AVHAIL fleet"},
    // ---- chart: bare commands ----
    {"K9AVT SNR?",     TextClass::DirectedCommand, "chart"},
    {"K9AVT INFO?",    TextClass::DirectedCommand, "chart"},
    {"K9AVT GRID?",    TextClass::DirectedCommand, "chart"},
    {"K9AVT STATUS?",  TextClass::DirectedCommand, "chart"},
    {"K9AVT HEARING?", TextClass::DirectedCommand, "chart"},
    {"K9AVT AGN?",     TextClass::DirectedCommand, "chart"},
    {"K9AVT QSL?",     TextClass::DirectedCommand, "chart"},
    {"K9AVT QSL",      TextClass::DirectedCommand, "chart"},
    {"K9AVT YES",      TextClass::DirectedCommand, "chart"},
    {"K9AVT NO",       TextClass::DirectedCommand, "chart"},
    {"K9AVT HW CPY?",  TextClass::DirectedCommand, "chart"},
    {"K9AVT RR",       TextClass::DirectedCommand, "chart"},
    {"K9AVT FB",       TextClass::DirectedCommand, "chart"},
    {"K9AVT 73",       TextClass::DirectedCommand, "chart"},
    {"K9AVT SK",       TextClass::DirectedCommand, "chart"},
    {"K9AVT DIT DIT",  TextClass::DirectedCommand, "chart"},
    {"K9AVT SNR -05",  TextClass::DirectedCommand, "SNR + report"},
    {"K9AVT SNR +10",  TextClass::DirectedCommand, "SNR + report"},
    {"K9AVT INFO QTH DENVER JOHN", TextClass::DirectedCommand, "INFO any arg"},
    {"K9AVT GRID DN18",            TextClass::DirectedCommand, "GRID any arg"},
    {"K9AVT TYPING",   TextClass::DirectedCommand, "typing macro"},
    {"K9AVT QUERY MSGS",  TextClass::DirectedCommand, "chart"},
    {"K9AVT QUERY MSG 42",TextClass::DirectedCommand, "chart"},
    {"K9AVT QUERY CALL K1ABC?", TextClass::DirectedCommand, "chart"},
    // ---- #93 family: conversational token + trailing text = FREE ----
    {"K9AVT NO PROBLEM SEE YOU LATER", TextClass::FreeText, "#93"},
    {"K9AVT 73 AND GOOD LUCK",         TextClass::FreeText, "#93"},
    {"K9AVT SNR WAS GREAT TODAY",      TextClass::FreeText, "#93"},
    {"K9AVT YES I AGREE WITH THAT",    TextClass::FreeText, "#93"},
    {"k9avt no problem see you later", TextClass::FreeText, "#93 lowercase"},
    {"K9AVT RR THAT AND MORE",         TextClass::FreeText, "#93"},
    // ---- exempt: ARQ explicitly wraps ----
    {"K9AVT MSG HELLO FROM THE OTHER SIDE", TextClass::ArqExempt, "MSG"},
    {"K9AVT MSG TO:K1ABC PLEASE CALL HOME", TextClass::ArqExempt, "MSG TO:"},
    {"K9AVT >K1ABC RELAY THIS PLEASE",      TextClass::ArqExempt, "relay"},
    // ---- [TODO #150] glued relay chains, incl. inner commands ----
    {"AC7WY>WM8Q/P HEARING?",               TextClass::ArqExempt, "relay glued + inner cmd"},
    {"WM8Q: AC7WY>WM8Q/P HEARING?",         TextClass::ArqExempt, "relay glued paste-back"},
    {"AC7WY>KJ7VWV>KL7UT>WD4KAV MSG HI",    TextClass::ArqExempt, "relay 3-hop glued"},
    {"AC7WY>KJ7VWV [MESSAGE]",              TextClass::ArqExempt, "relay-builder template"},
    {"K9AVT SNR?",                          TextClass::DirectedCommand, "plain cmd still excluded"},
    // ---- no ARQ to a group, ANY body (Andy 2026-07-17) ----
    {"wm8q: @PUBLIC MSG hi!",       TextClass::DirectedCommand, "group specimen"},
    {"@PUBLIC MSG HELLO THERE",     TextClass::DirectedCommand, "group MSG"},
    {"@PUBLIC MSG TO:K1ABC HELLO",  TextClass::DirectedCommand, "group MSG TO:"},
    {"@ALLCALL HELLO EVERYONE",     TextClass::DirectedCommand, "group freetext"},
    {"@ALLCALL",                    TextClass::DirectedCommand, "bare group"},
    // ---- plain free text ----
    {"K9AVT HELLO OLD FRIEND HOW GOES IT", TextClass::FreeText, "plain"},
    {"K9AVT",           TextClass::FreeText, "bare addressee"},
    {"HELLO ANYONE OUT THERE", TextClass::FreeText, "no addressee"},
    {"",                TextClass::FreeText, "empty"},
    {"K9AVT QUERY SOMETHING ODD PLEASE", TextClass::DirectedCommand, "QUERY = always command (policy)"},
};

struct PeerCase { char const *text; char const *want; char const *why; };
static PeerCase const peerCases[] = {
    {"WM8Q/P MSG CALL ME",      "WM8Q/P", "specimen (group selected)"},
    {"wm8q: wm8q/p msg call me","WM8Q/P", "paste-back + lowercase"},
    {"K9AVT HELLO",             "K9AVT",  "plain"},
    {"@ALLCALL QUERY MSGS",     "",       "group addressee = no peer"},
    {"HELLO EVERYONE",          "",       "no addressee"},
    {"",                        "",       "empty"},
};

// [BUILD 341 sendPeer] The rule EVERY consumer (gate, file resolver,
// startTx intercept) must share: selected individual wins; group/
// invalid/empty selection defers to the text's own addressee.
struct EffCase { char const *sel; char const *text; char const *want;
                 char const *why; };
static EffCase const effCases[] = {
    // THE bug (2026-07-17): group selected + text names its own peer
    // → enable gate said Armed but the send path resolved @ALLCALL
    // and shipped non-ARQ.
    {"@ALLCALL", "WM8Q/P MSG XXX XX",      "WM8Q/P", "specimen: group sel, text peer"},
    {"@ALLCALL", "WM8Q: WM8Q/P MSG XXX XX","WM8Q/P", "specimen paste-back form"},
    {"",         "WM8Q/P MSG XXX XX",      "WM8Q/P", "no selection, text peer"},
    {"K9AVT",    "WM8Q/P MSG XXX XX",      "K9AVT",  "valid selection WINS"},
    {"k9avt",    "hello there",            "K9AVT",  "lowercase selection normalized"},
    {"@ALLCALL", "HELLO EVERYONE",         "",       "group sel, no text peer"},
    {"@PUBLIC",  "@ALLCALL QUERY MSGS",    "",       "group sel, group text"},
    {"",         "",                       "",       "nothing anywhere"},
    {"QRZ?",     "K9AVT HELLO",            "K9AVT",  "non-callsign selection defers"},
};

static char const *name(TextClass c) {
    switch (c) {
    case TextClass::FreeText: return "FreeText";
    case TextClass::DirectedCommand: return "Command ";
    case TextClass::ArqExempt: return "Exempt  ";
    }
    return "?";
}

int main() {
    int fails = 0;
    for (auto const &c : cases) {
        TextClass const got = classifyOutgoingText(QString::fromUtf8(c.text));
        bool const ok = got == c.want;
        if (!ok) ++fails;
        printf("%s  want=%s got=%s  %-34s (%s)\n", ok ? "PASS" : "FAIL",
               name(c.want), name(got), c.text, c.why);
    }
    for (auto const &c : peerCases) {
        QString const got =
            ChunkedArq::leadingPeerOf(QString::fromUtf8(c.text));
        bool const ok = got == QString::fromUtf8(c.want);
        if (!ok) ++fails;
        printf("%s  peer want='%s' got='%s'  %-28s (%s)\n",
               ok ? "PASS" : "FAIL", c.want,
               got.toUtf8().constData(), c.text, c.why);
    }
    for (auto const &c : effCases) {
        QString const got = ChunkedArq::effectivePeer(
            QString::fromUtf8(c.sel), QString::fromUtf8(c.text));
        bool const ok = got == QString::fromUtf8(c.want);
        if (!ok) ++fails;
        printf("%s  eff sel='%s' want='%s' got='%s'  %-28s (%s)\n",
               ok ? "PASS" : "FAIL", c.sel, c.want,
               got.toUtf8().constData(), c.text, c.why);
    }
    printf("\n%d class + %d peer + %d eff cases, %d failures\n",
           (int)(sizeof(cases)/sizeof(cases[0])),
           (int)(sizeof(peerCases)/sizeof(peerCases[0])),
           (int)(sizeof(effCases)/sizeof(effCases[0])), fails);
    return fails ? 1 : 0;
}
