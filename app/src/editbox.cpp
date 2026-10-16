#include "editbox.h"
#include "audiobox.h"
#include "colorpicker.h"

#include "dialogs.h"
#include "grid.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "theme.h"
#include "videobox.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLocale>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFontDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStringListModel>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

// ---- TimeEdit ---------------------------------------------------------------

TimeEdit::TimeEdit(QWidget *parent) : QLineEdit(parent) {
    setFrames(false);
    connect(this, &QLineEdit::textEdited, this, [this] { emit edited(); });
}

void TimeEdit::setFrames(bool on) {
    m_frames = on;
    setInputMask(on ? QString() : "9:99:99.99;0");
    setFixedWidth(fontMetrics().horizontalAdvance("0:00:00.00") + 14);
}

void TimeEdit::setMs(qint64 ms, qint64 frame) {
    const int pos = cursorPosition();
    setText(m_frames ? QString::number(frame) : fs::fmtTime(ms));
    setCursorPosition(pos);
}

qint64 TimeEdit::ms() const { return m_frames ? -1 : fs::parseTime(text()); }
qint64 TimeEdit::frame() const {
    bool ok;
    const qint64 f = text().toLongLong(&ok);
    return ok ? f : -1;
}

void TimeEdit::wheelEvent(QWheelEvent *e) {
    const int step = e->angleDelta().y() > 0 ? 1 : -1;
    if (m_frames) setText(QString::number(std::max<qint64>(0, frame() + step)));
    else setText(fs::fmtTime(std::max<qint64>(0, ms() + step * 10)));
    emit edited();
}

// ---- text editor --------------------------------------------------------------

namespace {
// Aegisub's syntax highlighting (fs_core::syntax) in the theme's colours, misspellings squiggled
class TagHighlighter : public QSyntaxHighlighter {
public:
    TagHighlighter(QTextDocument *doc, const bool *templateLine) : QSyntaxHighlighter(doc), m_template(templateLine) {}
protected:
    void highlightBlock(const QString &t) override {
        using namespace theme;
        // by fs_core::syntax::Style; a curve's endpoints are its X and Y, underlined
        static constexpr Role roles[] = {SyntaxNormal,   SyntaxComment,    SyntaxDrawingCmd,      SyntaxDrawingX,
                                         SyntaxDrawingY, SyntaxDrawingX,   SyntaxDrawingY,        SyntaxBrackets,
                                         SyntaxSlashes,  SyntaxTags,       SyntaxError,           SyntaxParameters,
                                         SyntaxLineBreak, SyntaxKaraokeTemplate, SyntaxKaraokeVariable};
        // Aegisub's Subtitle/Highlight/Syntax; the misspellings are underlined either way
        const auto runs = QSettings().value("edit/syntax", true).toBool() ? fsb::syntax_styles(fs::Utf8(t), *m_template)
                                                                          : rust::Vec<uint32_t>{};
        int at = 0;
        for (size_t i = 0; i + 1 < runs.size(); i += 2) {
            const size_t style = std::min<size_t>(runs[i], std::size(roles) - 1);
            const int len = int(runs[i + 1]);
            QTextCharFormat f;
            f.setForeground(color(roles[style]));
            if (bold(roles[style])) f.setFontWeight(QFont::Bold);
            if (style == 5 || style == 6) f.setFontUnderline(true);
            if (roles[style] == SyntaxError) f.setBackground(color(SyntaxErrorBackground));
            setFormat(at, len, f);
            at += len;
        }
        if (auto *sp = spell::get()) {
            const auto bad = sp->misspelled(fs::Utf8(t));
            for (size_t i = 0; i + 1 < bad.size(); i += 2)
                for (int j = int(bad[i]); j < int(bad[i] + bad[i + 1]); ++j) {
                    QTextCharFormat f = format(j);
                    f.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
                    f.setUnderlineColor(color(SyntaxMisspelled));
                    setFormat(j, 1, f);
                }
        }
    }
private:
    const bool *m_template;
};

std::optional<rust::Box<fsb::Speller>> g_speller;
bool g_spellLoaded = false;
int g_spellGen = 0;
QString g_spellLang; // the dictionary actually loaded
std::optional<rust::Box<fsb::Thesaurus>> g_thes;
QString g_thesLang = "?"; // the language g_thes was loaded for; none tried yet

const QStringList kTags = {
    "an", "b", "i", "u", "s", "bord", "xbord", "ybord", "shad", "xshad", "yshad", "be", "blur", "fn", "fs",
    "fscx", "fscy", "fsp", "frx", "fry", "frz", "fax", "fay", "fe", "c", "1c", "2c", "3c", "4c", "alpha",
    "1a", "2a", "3a", "4a", "k", "K", "kf", "ko", "q", "r", "pos", "move", "org", "fad", "fade", "clip",
    "iclip", "p", "pbo", "t"};

// UTF-8 byte offset of a QString index (the core splits at byte positions).
size_t bytePos(const QString &s, int index) { return size_t(s.left(index).toUtf8().size()); }
} // namespace

QString spell::dir() {
    return QSettings().value("spell/dir", QCoreApplication::applicationDirPath() + "/dictionaries").toString();
}

QStringList spell::languages() {
    QStringList out;
    for (const QFileInfo &fi : QDir(dir()).entryInfoList({"*.dic"}, QDir::Files, QDir::Name))
        if (QFileInfo::exists(fi.absolutePath() + "/" + fi.completeBaseName() + ".aff")) out << fi.completeBaseName();
    return out;
}

// "polski (pl_PL)", or the code where Qt doesn't know the language
static QString languageLabel(const QString &code) {
    const QLocale l(code);
    return l.language() == QLocale::C ? code : QString("%1 (%2)").arg(l.nativeLanguageName(), code);
}

void spell::fillLanguages(QComboBox *box, const QString &current) {
    box->clear();
    for (const QString &code : languages()) box->addItem(languageLabel(code), code);
    box->setCurrentIndex(std::max(0, box->findData(current)));
}

fsb::Speller *spell::get() {
    // settings are read once here: every change of spell/* calls reload()
    if (!g_spellLoaded) {
        g_spellLoaded = true;
        QSettings st;
        if (!st.value("spell/enabled", true).toBool()) return nullptr;
        const QStringList langs = languages();
        QString lang = st.value("spell/lang").toString();
        if (!langs.contains(lang)) lang = langs.contains("pl_PL") ? "pl_PL" : langs.value(0);
        g_spellLang = lang;
        if (!lang.isEmpty()) {
            const QString base = dir() + "/" + lang;
            try {
                g_speller = fsb::new_speller(fs::Utf8(base + ".aff"), fs::Utf8(base + ".dic"));
                for (const QString &w : st.value("spell/user/" + lang).toStringList()) (*g_speller)->add_word(fs::Utf8(w));
            } catch (const std::exception &e) {
                qWarning("spell: %s", e.what());
            }
        }
    }
    return g_speller ? &**g_speller : nullptr;
}

int spell::generation() { return g_spellGen; }

void spell::reload() {
    g_speller.reset();
    g_spellLoaded = false;
    g_thesLang = "?"; // the folder may have changed
    ++g_spellGen;
    for (QWidget *w : QApplication::allWidgets())
        if (auto *e = qobject_cast<SubsTextEdit *>(w)) {
            if (auto *h = e->document()->findChild<QSyntaxHighlighter *>()) h->rehighlight();
        } else if (auto *g = qobject_cast<Grid *>(w)) {
            g->viewport()->update(); // it underlines misspellings too
        }
}

namespace {
struct Limits {
    int cps, cpsError, length, lines;
};
Limits loadLimits() {
    QSettings st;
    fsb::set_cps_ignore(st.value("subs/cpsIgnoreWhitespace", true).toBool(),
                        st.value("subs/cpsIgnorePunctuation", true).toBool());
    return {st.value("subs/maxCps", 15).toInt(), st.value("subs/cpsError", 30).toInt(),
            st.value("subs/maxLineLength", 43).toInt(), st.value("subs/maxLines", 2).toInt()};
}
Limits &g_limits() {
    static Limits l = loadLimits(); // read per painted cell, so cached
    return l;
}
} // namespace

int limits::cps() { return g_limits().cps; }

// Aegisub's grid: from nothing at the warning to full at the error threshold
double limits::cpsRed(int cps) {
    const int lo = g_limits().cps, hi = std::max(lo, g_limits().cpsError);
    return cps <= lo ? 0 : std::min(double(cps - lo + 1) / (hi - lo + 1), 1.0);
}

bool limits::wrapsOver(const QString &wraps) {
    const auto parts = wraps.split('/');
    return parts.size() > g_limits().lines ||
           std::ranges::any_of(parts, [](const QString &v) { return v.toInt() > g_limits().length; });
}

void limits::reload() { g_limits() = loadLimits(); }

void spell::addWord(const QString &word) {
    QSettings st;
    const QString key = "spell/user/" + g_spellLang;
    st.setValue(key, st.value(key).toStringList() << word);
    reload(); // reload applies the user list
}

QStringList spell::userWords() { return QSettings().value("spell/user/" + g_spellLang).toStringList(); }

void spell::removeWord(const QString &word) {
    QStringList words = userWords();
    if (!words.removeAll(word)) return;
    QSettings().setValue("spell/user/" + g_spellLang, words);
    reload(); // a speller can't forget a word, so it is built again without it
}

// MyThes thesauri (th_<lang>.idx + .dat) in the dictionary folder, as Aegisub finds them.
namespace {
namespace thesaurus {
QStringList languages() {
    QStringList out;
    for (const QFileInfo &fi : QDir(spell::dir()).entryInfoList({"th_*.idx"}, QDir::Files, QDir::Name))
        if (QFileInfo::exists(fi.absolutePath() + "/" + fi.completeBaseName() + ".dat")) out << fi.completeBaseName().mid(3);
    return out;
}

// The chosen one ("" disabled); until one is chosen, the first of the spell checker's language.
QString language() {
    QSettings st;
    if (st.contains("thesaurus/lang")) return st.value("thesaurus/lang").toString();
    for (const QString &l : languages())
        if (!g_spellLang.isEmpty() && l.startsWith(g_spellLang)) return l;
    return {};
}

fsb::Thesaurus *get() {
    if (const QString lang = language(); lang != g_thesLang) {
        g_thesLang = lang;
        g_thes.reset();
        const QString base = spell::dir() + "/th_" + lang;
        if (!lang.isEmpty()) {
            try {
                g_thes.emplace(fsb::new_thesaurus(fs::Utf8(base + ".idx"), fs::Utf8(base + ".dat")));
            } catch (const std::exception &e) {
                qWarning("thesaurus: %s", e.what());
            }
        }
    }
    return g_thes ? &**g_thes : nullptr;
}
} // namespace thesaurus
} // namespace

// Suggestions and "Add to dictionary" for a misspelled word under the mouse, and Aegisub's thesaurus.
void SubsTextEdit::contextMenuEvent(QContextMenuEvent *e) {
    std::unique_ptr<QMenu> m(createStandardContextMenu(e->pos()));
    QAction *const first = m->actions().value(0); // the standard actions follow ours
    const QTextCursor at = cursorForPosition(e->pos());
    const QTextBlock block = at.block();
    const QString t = block.text();
    // replaces t's [s, s + l) and selects the new text
    auto replacer = [this, block](int s, int l) {
        return [this, block, s, l](const QString &with) {
            QTextCursor c(block);
            c.setPosition(block.position() + s);
            c.setPosition(block.position() + s + l, QTextCursor::KeepAnchor);
            c.insertText(with);
            c.setPosition(block.position() + s, QTextCursor::KeepAnchor);
            setTextCursor(c);
        };
    };
    if (auto *sp = spell::get()) {
        const auto bad = sp->misspelled(fs::Utf8(t));
        for (size_t i = 0; i + 1 < bad.size(); i += 2) {
            const int s = int(bad[i]), l = int(bad[i + 1]);
            if (at.positionInBlock() < s || at.positionInBlock() > s + l) continue;
            const QString word = t.mid(s, l);
            const auto replace = replacer(s, l);
            const auto sugg = sp->suggest(fs::Utf8(word));
            for (size_t k = 0; k < std::min<size_t>(sugg.size(), 8); ++k) {
                const QString w = fs::q(sugg[k]);
                auto *a = new QAction(w, m.get());
                QFont f = a->font();
                f.setBold(true);
                a->setFont(f);
                connect(a, &QAction::triggered, this, [replace, w] { replace(w); });
                m->insertAction(first, a);
            }
            if (sugg.empty()) {
                auto *none = new QAction(tr("(no suggestions)"), m.get());
                none->setEnabled(false);
                m->insertAction(first, none);
            }
            auto *add = new QAction(tr("Add \"%1\" to dictionary").arg(word), m.get());
            connect(add, &QAction::triggered, this, [word] { spell::addWord(word); });
            m->insertAction(first, add);
            m->insertSeparator(first);
            break;
        }
    }
    const auto words = fsb::text_words(fs::Utf8(t));
    // Aegisub: a word of the user dictionary can leave it, and the language is chosen here
    for (size_t i = 0; i + 1 < words.size(); i += 2) {
        const int s = int(words[i]), l = int(words[i + 1]);
        if (at.positionInBlock() < s || at.positionInBlock() > s + l) continue;
        if (const QString word = t.mid(s, l); spell::get() && spell::userWords().contains(word)) {
            auto *remove = new QAction(tr("Remove \"%1\" from dictionary").arg(word), m.get());
            connect(remove, &QAction::triggered, this, [word] { spell::removeWord(word); });
            m->insertAction(first, remove);
        }
        break;
    }
    if (const QStringList langs = spell::languages(); !langs.isEmpty()) {
        auto *langMenu = new QMenu(tr("Spell checker language"), m.get());
        const QString cur = spell::get() ? g_spellLang : QString();
        for (const QString &lang : QStringList{QString()} + langs) {
            auto *a = langMenu->addAction(lang.isEmpty() ? tr("Disable") : languageLabel(lang), [lang] {
                QSettings st;
                st.setValue("spell/enabled", !lang.isEmpty());
                if (!lang.isEmpty()) st.setValue("spell/lang", lang);
                spell::reload();
            });
            a->setCheckable(true);
            a->setChecked(lang == cur);
        }
        m->insertMenu(first, langMenu);
        m->insertSeparator(first);
    }
    if (const QStringList langs = thesaurus::languages(); !langs.isEmpty())
        for (size_t i = 0; i + 1 < words.size(); i += 2) {
            const int s = int(words[i]), l = int(words[i + 1]);
            if (at.positionInBlock() < s || at.positionInBlock() > s + l) continue;
            const QString word = t.mid(s, l);
            const auto replace = replacer(s, l);
            const auto use = [this, replace](QMenu *menu, const QString &suggestion) {
                menu->addAction(suggestion, this, [replace, suggestion] {
                    replace(fs::q(fsb::thesaurus_word(fs::Utf8(suggestion))));
                });
            };
            auto *th = thesaurus::get();
            const auto entries = th ? th->lookup(fs::Utf8(word)) : rust::Vec<fsb::ThesEntry>();
            if (entries.empty()) {
                auto *none = new QAction(tr("No thesaurus suggestions"), m.get());
                none->setEnabled(false);
                m->insertAction(first, none);
            } else {
                auto *menu = new QMenu(tr("Thesaurus suggestions for \"%1\"").arg(word), m.get());
                for (const auto &en : entries) {
                    if (en.synonyms.empty()) {
                        use(menu, fs::q(en.meaning));
                        continue;
                    }
                    auto *sub = menu->addMenu(fs::q(en.meaning));
                    for (const auto &syn : en.synonyms) use(sub, fs::q(syn));
                }
                m->insertMenu(first, menu);
            }
            auto *langMenu = new QMenu(tr("Thesaurus language"), m.get());
            const QString cur = thesaurus::language();
            for (const QString &lang : QStringList{QString()} + langs) {
                auto *a = langMenu->addAction(lang.isEmpty() ? tr("Disable") : lang,
                                              [lang] { QSettings().setValue("thesaurus/lang", lang); });
                a->setCheckable(true);
                a->setChecked(lang == cur);
            }
            m->insertMenu(first, langMenu);
            m->insertSeparator(first);
            break;
        }
    // Aegisub's split commands
    m->addSeparator();
    m->addAction(tr("Split at cursor (estimate times)"), this, [this] { emit splitRequested(-1); });
    m->addAction(tr("Split at cursor (preserve times)"), this, [this] { emit splitRequested(-2); });
    m->addAction(tr("Split at cursor (at video frame)"), this, [this] { emit splitRequested(0); });
    m->addAction(tr("Revert"), this, [this] { emit revertRequested(); });
    m->addAction(tr("Insert original"), this, [this] { emit insertOriginalRequested(); });
    m->addSeparator();
    auto *quotes = m->addAction(tr("Automatically change quotes"));
    quotes->setCheckable(true);
    quotes->setChecked(QSettings().value("edit/changeQuotes", false).toBool());
    connect(quotes, &QAction::toggled, this, [](bool on) { QSettings().setValue("edit/changeQuotes", on); });
    m->exec(e->globalPos());
}

// Kainote: Shift+wheel over a number (a tag's value) steps it by 1, or by 0.1 right of its point.
void SubsTextEdit::wheelEvent(QWheelEvent *e) {
    const int delta = e->angleDelta().y() ? e->angleDelta().y() : e->angleDelta().x(); // Shift may turn it
    if (e->modifiers() != Qt::ShiftModifier || !delta) return QPlainTextEdit::wheelEvent(e);
    const QString t = toPlainText();
    const int at = cursorForPosition(e->position().toPoint()).position();
    const auto numeric = [&t](int i) { return t[i].isDigit() || t[i] == '.' || t[i] == '-'; };
    int s = at, end = at;
    while (s > 0 && numeric(s - 1)) --s;
    while (end < t.size() && numeric(end)) ++end;
    // not the digits of a colour or alpha (&H0000FF&)
    int hex = s;
    while (hex > 0 && isxdigit(t[hex - 1].toLatin1())) --hex;
    bool ok = s < end && !(hex >= 2 && t.mid(hex - 2, 2).compare("&H", Qt::CaseInsensitive) == 0);
    const double v = ok ? t.mid(s, end - s).toDouble(&ok) : 0;
    if (!ok) return e->accept();
    const int dot = int(t.indexOf('.', s));
    const double step = (dot >= 0 && dot < at && dot < end ? 0.1 : 1) * (delta > 0 ? 1 : -1);
    QTextCursor c(document());
    c.setPosition(s);
    c.setPosition(end, QTextCursor::KeepAnchor);
    c.insertText(QString::number(std::round((v + step) * 1000) / 1000 + 0.0, 'g', 12)); // + 0.0: no "-0"
    e->accept();
}

SubsTextEdit::SubsTextEdit(QWidget *parent) : QPlainTextEdit(parent) {
    m_highlighter = new TagHighlighter(document(), &m_templateLine);
    const auto colors = [this] {
        QPalette pal = palette();
        pal.setColor(QPalette::Base, theme::color(theme::SyntaxBackground));
        pal.setColor(QPalette::Text, theme::color(theme::SyntaxNormal));
        setPalette(pal);
        m_highlighter->rehighlight();
    };
    colors();
    connect(theme::notifier(), &theme::Notifier::changed, this, colors);
    setTabChangesFocus(true);
    m_completer = new QCompleter(new QStringListModel(kTags, this), this);
    m_completer->setWidget(this);
    m_completer->setCaseSensitivity(Qt::CaseSensitive);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this, [this](const QString &c) {
        auto cur = textCursor();
        cur.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, int(m_completer->completionPrefix().size()));
        cur.insertText(c);
        setTextCursor(cur);
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &SubsTextEdit::updateCalltip);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &SubsTextEdit::matchBraces);
}

// Kainote's brace matching: the bracket at the cursor, else the one just before it, and its pair
void SubsTextEdit::matchBraces() {
    QList<QTextEdit::ExtraSelection> marks;
    const QString t = toPlainText();
    const int at = textCursor().position();
    static const QString opening = "{(", closing = "})";
    for (const int p : {at, at - 1}) {
        if (p < 0 || p >= t.size()) continue;
        const int o = int(opening.indexOf(t[p])), c = int(closing.indexOf(t[p]));
        if (o < 0 && c < 0) continue;
        const QChar other = o >= 0 ? closing[o] : opening[c];
        const int step = o >= 0 ? 1 : -1;
        int q = p;
        for (int depth = 0; q >= 0 && q < t.size(); q += step)
            if (t[q] == t[p]) ++depth;
            else if (t[q] == other && --depth == 0) break;
        if (q >= 0 && q < t.size())
            for (const int m : {p, q}) {
                QTextEdit::ExtraSelection s;
                s.cursor = QTextCursor(document());
                s.cursor.setPosition(m);
                s.cursor.setPosition(m + 1, QTextCursor::KeepAnchor);
                s.format.setBackground(theme::color(theme::SyntaxBraces));
                marks << s;
            }
        break;
    }
    setExtraSelections(marks);
}

// Aegisub's call tips: the tag's parameters under it, the one at the cursor in bold
void SubsTextEdit::updateCalltip() {
    rust::String tip;
    const auto r = hasFocus() && QSettings().value("edit/callTips", false).toBool()
                       ? fsb::calltip(fs::Utf8(toPlainText()), size_t(textCursor().position()), tip)
                       : rust::Vec<size_t>{};
    if (r.empty()) {
        if (m_calltip) m_calltip->hide();
        return;
    }
    if (!m_calltip) {
        m_calltip = new QLabel(this, Qt::ToolTip);
        m_calltip->setTextFormat(Qt::RichText);
        m_calltip->setForegroundRole(QPalette::ToolTipText);
        m_calltip->setBackgroundRole(QPalette::ToolTipBase);
        m_calltip->setAutoFillBackground(true);
        m_calltip->setMargin(3);
    }
    const QString p = fs::q(tip);
    const int a = int(r[0]), b = int(r[1]);
    m_calltip->setText("<nobr>" + p.left(a).toHtmlEscaped() + "<b>" + p.mid(a, b - a).toHtmlEscaped() + "</b>" +
                       p.mid(b).toHtmlEscaped() + "</nobr>");
    m_calltip->adjustSize();
    QTextCursor slash(document());
    slash.setPosition(int(r[2]) - 1);
    m_calltip->move(viewport()->mapToGlobal(cursorRect(slash).bottomLeft() + QPoint(0, 2)));
    m_calltip->show();
}

void SubsTextEdit::focusOutEvent(QFocusEvent *e) {
    if (m_calltip) m_calltip->hide();
    QPlainTextEdit::focusOutEvent(e);
}

void SubsTextEdit::setText(const QString &t) {
    if (t == toPlainText()) return;
    const int pos = textCursor().position();
    setPlainText(t);
    auto c = textCursor();
    c.setPosition(std::min<int>(pos, int(t.size())));
    setTextCursor(c);
}

namespace {
// Matches of `re` outside \t(...): Aegisub reads and edits a block's own tags, not animated ones.
QList<QRegularExpressionMatch> topLevel(const QString &text, const QRegularExpression &re) {
    QList<QRegularExpressionMatch> out;
    int depth = 0, at = 0;
    for (auto it = re.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        for (; at < m.capturedStart(); ++at)
            depth = text[at] == '{' ? 0 : text[at] == '(' ? depth + 1 : text[at] == ')' ? depth - 1 : depth;
        if (depth <= 0) out << m;
    }
    return out;
}
} // namespace

// The override block Aegisub edits at `pos`: the one containing or touching it, else {-1, -1}.
std::pair<int, int> SubsTextEdit::blockAt(int pos) const {
    const QString t = toPlainText();
    const int open = pos > 0 ? int(t.lastIndexOf('{', pos - 1)) : -1;
    if (open > (pos > 0 ? int(t.lastIndexOf('}', pos - 1)) : -1)) {
        const int close = int(t.indexOf('}', pos));
        return close >= 0 ? std::pair{open, close} : std::pair{-1, -1};
    }
    if (pos > 0 && t[pos - 1] == '}' && open >= 0) return {open, pos - 1};
    if (pos < t.size() && t[pos] == '{')
        if (const int close = int(t.indexOf('}', pos)); close >= 0) return {pos, close};
    return {-1, -1};
}

QString SubsTextEdit::tagValue(const QRegularExpression &re, int pos) const {
    const auto [open, close] = blockAt(pos);
    const auto ms = topLevel(toPlainText().left(open >= 0 ? close + 1 : pos), re);
    return ms.isEmpty() ? QString() : ms.last().captured(1);
}

// Insert `\tag` into the override block at the cursor, or a new `{\tag}`;
// an existing tag of the same name in that block is replaced.
void SubsTextEdit::insertTag(const QString &tag) {
    static const QRegularExpression nameRe("^\\\\(\\d?[a-zA-Z]+)");
    const QString name = nameRe.match(tag).captured(1);
    QString t = toPlainText();
    int pos = textCursor().position();
    if (const auto [open, close] = blockAt(pos); open >= 0) {
        QString blk = t.mid(open + 1, close - open - 1);
        // \c is \1c; a font name runs into its tag name
        const QString re = name == "1c" ? QString("1?c(?![a-zA-Z])")
                           : name == "fn" ? QString("fn")
                                          : QRegularExpression::escape(name) + "(?![a-zA-Z])";
        const auto ms = topLevel(blk, QRegularExpression("\\\\" + re + "[^\\\\}]*"));
        for (auto i = ms.size(); i-- > 0;)
            blk.replace(ms[i].capturedStart(), ms[i].capturedLength(), i ? QString() : tag);
        if (ms.isEmpty()) blk += tag;
        t.replace(open + 1, close - open - 1, blk);
        pos = open + 1 + int(blk.size()) + 1;
    } else {
        t.insert(pos, "{" + tag + "}");
        pos += int(tag.size()) + 2;
    }
    setPlainText(t);
    auto c = textCursor();
    c.setPosition(pos);
    setTextCursor(c);
}

// \b, \i, \u, \s: flip the state in effect (the style's unless a tag sets it) at the cursor,
// or around the selection.
void SubsTextEdit::toggleTag(const QString &name, bool styleOn) {
    auto c = textCursor();
    const QString v = tagValue(QRegularExpression("\\\\" + name + "(\\d+)(?![0-9a-zA-Z])"), c.selectionStart());
    const bool on = v.isEmpty() ? styleOn : v != "0";
    if (c.hasSelection()) {
        c.insertText(QString("{\\%1%2}%3{\\%1%4}").arg(name, on ? "0" : "1", c.selectedText(), on ? "1" : "0"));
        return;
    }
    insertTag("\\" + name + (on ? "0" : "1"));
}

void SubsTextEdit::keyPressEvent(QKeyEvent *e) {
    if (m_completer->popup()->isVisible() &&
        (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Tab ||
         e->key() == Qt::Key_Escape)) {
        e->ignore();
        return;
    }
    // Kainote's quote changing: " opens with „ and closes an open one with ”
    if (e->text() == "\"" && QSettings().value("edit/changeQuotes", false).toBool()) {
        const QString before = toPlainText().left(textCursor().position());
        return insertPlainText(before.lastIndexOf(QChar(0x201E)) > before.lastIndexOf(QChar(0x201D)) ? QChar(0x201D)
                                                                                                     : QChar(0x201E));
    }
    if (auto *mw = qobject_cast<MainWindow *>(window()))
        if (const char *id = mw->panelCommand(e, EditBox::keys())) return emit keyCommand(id);
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) return; // a line breaks with \N, not Enter
    QPlainTextEdit::keyPressEvent(e);
    // tag autocomplete after a backslash
    const QString before = toPlainText().left(textCursor().position());
    static const QRegularExpression tail("\\\\([0-9a-zA-Z]*)$");
    const auto m = tail.match(before);
    if (m.hasMatch() && before.lastIndexOf('{') > before.lastIndexOf('}')) {
        m_completer->setCompletionPrefix(m.captured(1));
        QRect r = cursorRect();
        r.setWidth(m_completer->popup()->sizeHintForColumn(0) + 30);
        m_completer->complete(r);
    } else {
        m_completer->popup()->hide();
    }
}

// ---- EditBox ------------------------------------------------------------------

EditBox::EditBox(TabPanel *tab) : QWidget(tab), m_tab(tab) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(3);

    // tag bar
    m_tagBar = new QHBoxLayout;
    m_tagBar->setSpacing(2);
    auto tool = [this](const QString &text, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        m_tagBar->addWidget(b);
        return b;
    };
    connect(tool("F", tr("Font selection")), &QToolButton::clicked, this, [this] {
        // Aegisub: start from the font in effect at the cursor and set only what changed
        const auto st = lineStyle();
        const int pos = m_text->textCursor().position();
        const auto value = [&](const QString &n) {
            return m_text->tagValue(QRegularExpression("\\\\" + n + (n == "fn" ? "([^\\\\}]+)" : "([\\d.]+)")), pos);
        };
        QFont start(value("fn").isEmpty() ? fs::q(st.font) : value("fn"));
        if (const double size = value("fs").isEmpty() ? st.size : value("fs").toDouble(); size > 0)
            start.setPointSizeF(size);
        const bool styleFlags[] = {st.bold, st.italic, st.underline, st.strikeout};
        bool was[4];
        for (int k = 0; k < 4; ++k) {
            const QString v = value(QString(QLatin1Char("bius"[k])) + "(?=\\d)");
            was[k] = v.isEmpty() ? styleFlags[k] : v.toDouble() != 0;
        }
        start.setBold(was[0]);
        start.setItalic(was[1]);
        start.setUnderline(was[2]);
        start.setStrikeOut(was[3]);
        bool ok;
        const QFont f = QFontDialog::getFont(&ok, start, this);
        if (!ok) return;
        if (f.family() != start.family()) m_text->insertTag("\\fn" + f.family());
        if (f.pointSizeF() != start.pointSizeF()) m_text->insertTag("\\fs" + QString::number(f.pointSizeF()));
        const bool now[] = {f.bold(), f.italic(), f.underline(), f.strikeOut()};
        for (int k = 0; k < 4; ++k)
            if (now[k] != was[k]) m_text->insertTag(QString("\\%1%2").arg(QLatin1Char("bius"[k])).arg(int(now[k])));
    });
    const char *toggles[][2] = {{"b", QT_TR_NOOP("Bold")}, {"i", QT_TR_NOOP("Italic")}, {"u", QT_TR_NOOP("Underline")},
                                {"s", QT_TR_NOOP("Strikethrough")}};
    for (auto &t : toggles) {
        auto *b = tool(QString(t[0]).toUpper(), tr(t[1]));
        QFont f = b->font();
        f.setBold(t[0][0] == 'b');
        f.setItalic(t[0][0] == 'i');
        f.setUnderline(t[0][0] == 'u');
        f.setStrikeOut(t[0][0] == 's');
        b->setFont(f);
        connect(b, &QToolButton::clicked, this, [this, n = QString(t[0])] { toggleTag(n); });
    }
    const char *colors[] = {QT_TR_NOOP("Primary color"), QT_TR_NOOP("Secondary color for karaoke"),
                            QT_TR_NOOP("Border color"), QT_TR_NOOP("Shadow color")};
    const QColor bars[] = {QColor(0xf0, 0xf0, 0xf0), QColor(0xe0, 0x50, 0x50), QColor(0x20, 0x20, 0x20),
                           QColor(0x80, 0x80, 0x80)};
    for (int k = 0; k < 4; ++k) {
        auto *b = tool(QString(), tr(colors[k]) + QString(" (\\%1c)").arg(k + 1));
        QPixmap px(40, 40);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing);
        QFont f = font();
        f.setBold(true);
        f.setPixelSize(30);
        p.setFont(f);
        p.setPen(QColor(0x5a, 0x8c, 0xe6));
        p.drawText(QRect(0, -4, 40, 36), Qt::AlignCenter, "A");
        p.fillRect(QRectF(6, 33, 28, 6), bars[k]);
        p.setPen(QColor(0x5a, 0x8c, 0xe6));
        p.drawRect(QRectF(6, 33, 28, 6));
        p.end();
        b->setIcon(QIcon(px));
        connect(b, &QToolButton::clicked, this, [this, k] { setColorTag(k + 1); });
    }
    m_an = new QComboBox(this);
    m_an->setToolTip(tr("Text position"));
    const char *an[] = {QT_TR_NOOP("Bottom-left"), QT_TR_NOOP("Bottom-center"), QT_TR_NOOP("Bottom-right"),
                        QT_TR_NOOP("Middle-left"), QT_TR_NOOP("Middle-center"), QT_TR_NOOP("Middle-right"),
                        QT_TR_NOOP("Top-left"), QT_TR_NOOP("Top-center"), QT_TR_NOOP("Top-right")};
    for (int k = 0; k < 9; ++k) m_an->addItem(QString("%1 (an%2)").arg(tr(an[k])).arg(k + 1));
    m_an->setCurrentIndex(1);
    connect(m_an, &QComboBox::activated, this, [this](int k) {
        m_tab->s().set_tags(fs::slice(m_tab->selection()), fs::Utf8(QString("\\an%1").arg(k + 1)), "");
        m_tab->changed();
    });
    m_tagBar->addWidget(m_an);
    m_userTags = new QHBoxLayout;
    m_userTags->setSpacing(2);
    m_tagBar->addLayout(m_userTags);
    m_tagBar->addStretch();
    v->addLayout(m_tagBar);
    rebuildTagButtons();

    // info row
    auto *info = new QHBoxLayout;
    m_lineLabel = new QLabel(this);
    m_wrapsLabel = new QLabel(this);
    m_wrapsLabel->setToolTip(tr("About 43 characters per line and at most 2 lines"));
    m_cpsLabel = new QLabel(this);
    m_tlMode = new QCheckBox(tr("Translator mode"), this);
    m_timeRadio = new QRadioButton(tr("Time"), this);
    m_frameRadio = new QRadioButton(tr("Frames"), this);
    m_timeRadio->setChecked(true);
    for (QWidget *w : {static_cast<QWidget *>(m_lineLabel), static_cast<QWidget *>(m_wrapsLabel),
                       static_cast<QWidget *>(m_cpsLabel)})
        info->addWidget(w);
    info->addSpacing(8);
    info->addWidget(m_tlMode);
    info->addWidget(m_timeRadio);
    info->addWidget(m_frameRadio);
    info->addStretch();
    v->addLayout(info);
    connect(m_tlMode, &QCheckBox::clicked, this, [this](bool on) {
        if (!on && !m_tab->main()->confirm(tr("Turning translation mode off deletes the original text. Continue?"))) {
            m_tlMode->setChecked(true);
            return;
        }
        m_tab->s().tl_enable(on);
        m_tab->changed();
    });
    connect(m_frameRadio, &QRadioButton::toggled, this, [this](bool on) {
        m_framesMode = on;
        for (auto *t : {m_start, m_end, m_duration}) t->setFrames(on);
        reload();
        m_tab->grid()->viewport()->update();
    });

    // TL original + buttons
    m_tlBox = new QWidget(this);
    auto *tlv = new QVBoxLayout(m_tlBox);
    tlv->setContentsMargins(0, 0, 0, 0);
    m_original = new SubsTextEdit(m_tlBox);
    m_original->setFont(QSettings().value("edit/font", m_original->font()).value<QFont>());
    tlv->addWidget(m_original, 1);
    auto *tlb = new QHBoxLayout;
    auto *pasteSel = new QPushButton(tr("Paste the selected"), m_tlBox);
    auto *pasteAll = new QPushButton(tr("Paste all"), m_tlBox);
    m_hideOriginal = new QCheckBox(tr("Hide original"), m_tlBox);
    m_doubtful = new QCheckBox(tr("Not confirmed"), m_tlBox);
    m_movingTags = new QCheckBox(tr("Moving tags"), m_tlBox);
    for (QWidget *w : std::initializer_list<QWidget *>{pasteSel, pasteAll, m_hideOriginal, m_doubtful, m_movingTags})
        tlb->addWidget(w);
    tlb->addStretch();
    tlv->addLayout(tlb);
    v->addWidget(m_tlBox, 1);
    auto pasted = [this](QString t) {
        return m_movingTags->isChecked() ? t : fs::q(fsb::strip_tags(fs::Utf8(t)));
    };
    connect(pasteSel, &QPushButton::clicked, this, [this, pasted] {
        m_text->insertPlainText(pasted(m_original->textCursor().selectedText()));
    });
    connect(pasteAll, &QPushButton::clicked, this, [this, pasted] {
        m_text->setPlainText(pasted(m_original->text()));
        commit("text");
    });
    connect(m_hideOriginal, &QCheckBox::toggled, m_tab, &TabPanel::setHideOriginal);
    connect(m_doubtful, &QCheckBox::clicked, this, [this] { commit("doubtful"); });
    connect(m_original, &QPlainTextEdit::textChanged, this, [this] { commit("original"); });

    m_text = new SubsTextEdit(this);
    m_text->setFont(QSettings().value("edit/font", m_text->font()).value<QFont>());
    v->addWidget(m_text, 2);
    connect(m_text, &QPlainTextEdit::textChanged, this, [this] { commit("text"); });
    for (auto *e : {m_text, m_original})
        connect(e->document(), &QTextDocument::contentsChange, this, [this](int pos, int removed, int added) {
            if (m_loading || removed == added) return; // a format, from the highlighter
            const int kind = !removed ? 1 : !added ? 2 : 3;
            // typing on where the last edit ended, Delete or Backspace next to it, is the same step
            const bool on = kind == m_editKind && kind != 3 &&
                            (kind == 1 ? pos == m_editPos : pos == m_editPos || pos + removed == m_editPos);
            if (!on) ++m_undoStep;
            m_editKind = kind;
            m_editPos = kind == 1 ? pos + added : pos;
        });
    m_textInfo = new QLabel(this);
    m_textInfo->setTextFormat(Qt::PlainText);
    v->addWidget(m_textInfo);
    connect(m_text, &QPlainTextEdit::cursorPositionChanged, this, &EditBox::updateTextInfo);
    connect(m_text, &QPlainTextEdit::selectionChanged, this, &EditBox::updateTextInfo);
    connect(m_text, &QPlainTextEdit::textChanged, this, &EditBox::updateTextInfo);
    for (auto *e : {m_text, m_original})
        connect(e, &SubsTextEdit::keyCommand, this, [this, e](const char *id) { runCommand(id, e); });
    for (auto *e : {m_text, m_original})
        connect(e, &SubsTextEdit::splitRequested, this, [this, e](qint64 at) {
            // at the video frame, as Aegisub: the frame shown, kept within the line, ends the first part
            if (VideoBox *v = m_tab->video(); at == 0 && v->hasVideo() && m_row >= 0) {
                const fsb::Row r = m_tab->s().row(size_t(m_row));
                at = v->timeAtEnd(std::max(v->frameAtStart(r.start), std::min(v->frame(), v->frameAtEnd(r.end))));
            } else if (at == 0) {
                at = m_tab->videoTime();
            }
            splitAtCursor(at, e == m_original);
        });
    connect(m_text, &SubsTextEdit::revertRequested, this, [this] { m_text->setText(m_initial); });
    // Aegisub's edit/insert_original: the text from before the edits, at the cursor
    connect(m_text, &SubsTextEdit::insertOriginalRequested, this, [this] { m_text->insertPlainText(m_initial); });

    // fields row (wraps into two rows when narrow)
    m_fields1 = new QWidget(this);
    m_fields2 = new QWidget(this);
    m_row1 = new QHBoxLayout(m_fields1);
    m_row2 = new QHBoxLayout(m_fields2);
    for (auto *l : {m_row1, m_row2}) {
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(3);
    }
    m_comment = new QCheckBox(tr("Comment"), this);
    m_comment->setToolTip(tr("Comments are not shown"));
    auto spin = [this](const QString &tip, int max) {
        auto *s = new QSpinBox(this);
        s->setRange(0, max);
        s->setToolTip(tip);
        s->setButtonSymbols(QAbstractSpinBox::NoButtons);
        s->setFixedWidth(fontMetrics().horizontalAdvance("00000") + 10);
        return s;
    };
    m_layer = spin(tr("Layer: higher layers are drawn on top"), 99999);
    m_start = new TimeEdit(this);
    m_end = new TimeEdit(this);
    m_duration = new TimeEdit(this);
    m_start->setToolTip(tr("Start time"));
    m_end->setToolTip(tr("End time"));
    m_duration->setToolTip(tr("Duration"));
    m_style = new QComboBox(this);
    m_style->setMinimumContentsLength(10);
    m_styleEdit = new QToolButton(this);
    m_styleEdit->setText(tr("Edit"));
    m_actor = new QComboBox(this);
    m_actor->setEditable(true);
    m_actor->setMinimumContentsLength(8);
    m_actor->setToolTip(tr("Actor"));
    m_actor->lineEdit()->setPlaceholderText(tr("Actor"));
    m_ml = spin(tr("Left margin"), 9999);
    m_mr = spin(tr("Right margin"), 9999);
    m_mv = spin(tr("Vertical margin"), 9999);
    m_effect = new QComboBox(this);
    m_effect->setEditable(true);
    m_effect->setMinimumContentsLength(8);
    m_effect->setToolTip(tr("Effect"));
    m_effect->lineEdit()->setPlaceholderText(tr("Effect"));
    // Kainote order: comment, layer, times, style, edit | actor, margins, effect; the combos take the slack
    for (QWidget *w : std::initializer_list<QWidget *>{m_comment, m_layer, m_start, m_end, m_duration, m_style,
                                                       m_styleEdit})
        m_row1->addWidget(w, w == m_style ? 1 : 0);
    m_wrapWidgets = {m_actor, m_ml, m_mr, m_mv, m_effect};
    v->addWidget(m_fields1);
    v->addWidget(m_fields2);
    wrapFields();

    connect(m_comment, &QCheckBox::clicked, this, [this] { commit("comment"); });
    for (auto *s : {m_layer, m_ml, m_mr, m_mv})
        connect(s, &QSpinBox::valueChanged, this, [this, s] {
            commit(s == m_layer ? "layer" : s == m_ml ? "margin_l" : s == m_mr ? "margin_r" : "margin_v");
        });
    // Enter in any field goes to the next line, as in the text (Aegisub, Kainote)
    for (QWidget *w : std::initializer_list<QWidget *>{m_layer, m_ml, m_mr, m_mv, m_start, m_end, m_duration, m_style,
                                                       m_actor->lineEdit(), m_effect->lineEdit()})
        w->installEventFilter(this);
    connect(m_start, &TimeEdit::edited, this, [this] { commit("start"); });
    connect(m_end, &TimeEdit::edited, this, [this] { commit("end"); });
    connect(m_duration, &TimeEdit::edited, this, [this] { commit("duration"); });
    connect(m_style, &QComboBox::activated, this, [this] { commit("style"); });
    connect(m_actor, &QComboBox::currentTextChanged, this, [this] { commit("actor"); });
    connect(m_effect, &QComboBox::currentTextChanged, this, [this] { commit("effect"); });
    connect(m_styleEdit, &QToolButton::clicked, this, [this] {
        m_tab->main()->editStyle(m_style->currentText());
    });
    m_tlBox->setVisible(false);
}

bool EditBox::eventFilter(QObject *o, QEvent *e) {
    if (e->type() == QEvent::KeyPress) {
        const auto *k = static_cast<QKeyEvent *>(e);
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && !(k->modifiers() & ~Qt::KeypadModifier)) {
            nextLine();
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

void EditBox::wrapFields() {
    const bool narrow = width() < 850;
    auto *target = narrow ? m_row2 : m_row1;
    for (auto *w : m_wrapWidgets) target->addWidget(w, w == m_actor || w == m_effect ? 1 : 0);
    m_fields2->setVisible(narrow);
}

void EditBox::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    const bool narrow = width() < 850;
    if (narrow != m_fields2->isVisible()) wrapFields();
}

void EditBox::rebuildTagButtons() {
    while (auto *it = m_userTags->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    QSettings st;
    // Kainote starts with no tag buttons; the "+" button adds them
    QStringList defs = st.value("edit/tagButtons").toStringList();
    for (int k = 0; k < defs.size(); ++k) {
        const auto parts = defs[k].split('|');
        auto *b = new QToolButton(this);
        b->setText(parts.value(0, QString("T%1").arg(k + 1)));
        b->setToolTip(parts.value(1));
        b->setAutoRaise(true);
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QToolButton::customContextMenuRequested, this, [this, k] { configureTagButton(k); });
        connect(b, &QToolButton::clicked, this, [this, parts] {
            const QString tag = parts.value(1);
            const int mode = parts.value(2).toInt();
            if (mode == 2) m_text->insertPlainText(tag);
            else if (mode == 1) {
                auto c = m_text->textCursor();
                c.setPosition(m_text->text().startsWith('{') ? 1 : 0);
                m_text->setTextCursor(c);
                m_text->insertTag(tag);
            } else m_text->insertTag(tag);
        });
        m_userTags->addWidget(b);
    }
    auto *more = new QToolButton(this);
    more->setText("+");
    more->setToolTip(tr("Manage tag buttons"));
    more->setAutoRaise(true);
    connect(more, &QToolButton::clicked, this, [this] {
        QSettings st;
        auto defs = st.value("edit/tagButtons").toStringList();
        if (defs.size() >= 20) return;
        defs << QString("T%1|\\|0").arg(defs.size() + 1);
        st.setValue("edit/tagButtons", defs);
        configureTagButton(int(defs.size()) - 1);
    });
    m_userTags->addWidget(more);
}

void EditBox::configureTagButton(int index) {
    QSettings st;
    auto defs = st.value("edit/tagButtons").toStringList();
    if (index >= defs.size()) return;
    auto parts = defs[index].split('|');
    TagButtonDialog d(parts.value(0), parts.value(1), parts.value(2).toInt(), this);
    const int r = d.exec();
    if (r == TagButtonDialog::Delete) defs.removeAt(index);
    else if (r == QDialog::Accepted) defs[index] = d.name() + "|" + d.tag() + "|" + QString::number(d.mode());
    else return;
    st.setValue("edit/tagButtons", defs);
    rebuildTagButtons();
}

fsb::StyleData EditBox::lineStyle() const {
    for (const auto &st : m_tab->s().styles())
        if (fs::q(st.name) == m_style->currentText()) return st;
    return {};
}

void EditBox::toggleTag(const QString &name) {
    const auto st = lineStyle();
    m_text->toggleTag(name,
                      name == "b" ? st.bold : name == "i" ? st.italic : name == "u" ? st.underline : st.strikeout);
}

void EditBox::setColorTag(int which) {
    // Aegisub: the dialog starts from the colour in effect at the cursor, the style's unless a tag sets it
    const auto st = lineStyle();
    const rust::String *colors[] = {&st.primary, &st.secondary, &st.outline_color, &st.back};
    QColor start = st.name.empty() ? QColor(Qt::white) : assColor(fs::q(*colors[which - 1]));
    const int pos = m_text->textCursor().position();
    const QString color =
        which == 1 ? QString(R"(\\1?c&H([0-9A-Fa-f]+))") : QString(R"(\\%1c&H([0-9A-Fa-f]+))").arg(which);
    if (const QString c = m_text->tagValue(QRegularExpression(color), pos); !c.isEmpty()) {
        const QColor o = assColor(c);
        start.setRgb(o.red(), o.green(), o.blue(), start.alpha());
    }
    const QRegularExpression alpha(QString(R"(\\(?:%1a|alpha)&H([0-9A-Fa-f]+))").arg(which));
    if (const QString a = m_text->tagValue(alpha, pos); !a.isEmpty())
        start.setAlpha(255 - int(a.toUInt(nullptr, 16) & 0xff));
    // Aegisub: the line shows the colour as it is picked, in an undo step of its own that Cancel undoes
    const QString before = m_text->toPlainText();
    const int at = m_text->textCursor().position();
    const auto place = [this, at] {
        auto c = m_text->textCursor();
        c.setPosition(std::min(at, int(m_text->toPlainText().size())));
        m_text->setTextCursor(c);
    };
    const auto restore = [this, &before, place] {
        m_text->setPlainText(before);
        place();
    };
    ++m_undoStep;
    bool previewed = false;
    const QColor c = ColorPicker::get(this, start, true, [&](const QColor &col) {
        {
            const QSignalBlocker quiet(m_text); // the colour and its alpha as one change
            restore();
            m_text->insertTag(QString::asprintf("\\%dc&H%02X%02X%02X&", which, col.blue(), col.green(), col.red()));
            if (col.alpha() != start.alpha())
                m_text->insertTag(QString::asprintf("\\%da&H%02X&", which, 255 - col.alpha()));
        }
        previewed = true;
        emit m_text->textChanged();
    });
    if (!c.isValid() && previewed) {
        m_tab->undo();
        // a preview longer than the 30 s of a typing step spans two of them: the text goes back anyway
        if (m_text->toPlainText() != before) restore();
        place();
    }
    ++m_undoStep;
}

void EditBox::load(int row) {
    const bool moved = row != m_row;
    m_row = row;
    reload();
    if (moved && m_row >= 0) m_initial = fs::q(m_tab->s().row(size_t(m_row)).text);
}

void EditBox::updateTextInfo() {
    const QString t = m_text->toPlainText();
    const auto c = m_text->textCursor();
    const int pos = c.position();
    const QString before = t.left(pos);
    // \N counts as a line break, as on screen
    const qsizetype br = before.lastIndexOf("\\N");
    const qsizetype lineStart = std::max<qsizetype>(br < 0 ? 0 : br + 2, before.lastIndexOf('\n') + 1);
    m_textInfo->setText(tr("Length: %1     Lines: %2     Ln: %3     Col: %4     Sel: %5     Ch: %6")
                            .arg(t.size())
                            .arg(t.count("\\N") + t.count('\n') + 1)
                            .arg(before.count("\\N") + before.count('\n') + 1)
                            .arg(pos - lineStart + 1)
                            .arg(std::abs(c.anchor() - pos))
                            .arg(pos + 1));
}

void EditBox::reload() {
    auto &s = m_tab->s();
    if (m_row < 0 || size_t(m_row) >= s.row_count()) {
        m_row = s.row_count() ? 0 : -1;
        if (m_row < 0) return;
    }
    m_loading = true;
    m_initialTimes.clear();
    const fsb::Row r = s.row(size_t(m_row));
    const auto info = s.file_info();
    const QString fmt = fs::q(info.format);
    const bool ass = fmt == "ASS" || fmt == "SSA";
    m_tlMode->setEnabled(ass);
    m_tlMode->setChecked(info.tl_mode);
    m_tlBox->setVisible(info.tl_mode);
    // Aegisub: a comment whose effect starts with "template" is the karaoke templater's code
    const bool tpl = r.comment && fs::q(r.effect).startsWith("template", Qt::CaseInsensitive);
    m_text->setTemplateLine(tpl);
    m_original->setTemplateLine(tpl);
    m_text->setText(fs::q(r.text));
    m_original->setText(fs::q(r.original));
    m_doubtful->setChecked(r.doubtful);
    m_comment->setChecked(r.comment);
    m_layer->setValue(r.layer);
    if (m_framesMode && !m_tab->video()->hasVideo()) m_timeRadio->setChecked(true);
    showTimes(r, {});
    // style list
    m_style->clear();
    for (const auto &st : s.styles()) m_style->addItem(fs::q(st.name));
    m_style->setCurrentText(fs::q(r.style));
    const auto &ci = m_tab->grid()->columnInfo();
    auto fill = [](QComboBox *c, const rust::Vec<rust::String> &items, const QString &cur) {
        QStringList l;
        for (const auto &v : items) l << fs::q(v);
        l.sort();
        c->clear();
        c->addItems(l);
        c->setCurrentText(cur);
    };
    fill(m_actor, ci.actors, fs::q(r.actor));
    fill(m_effect, ci.effects, fs::q(r.effect));
    m_ml->setValue(r.margin_l);
    m_mr->setValue(r.margin_r);
    m_mv->setValue(r.margin_v);
    for (QWidget *w : std::initializer_list<QWidget *>{m_style, m_styleEdit, m_actor, m_ml, m_mr, m_mv, m_effect,
                                                       m_layer, m_an})
        w->setEnabled(ass);
    updateInfo(r);
    m_loading = false;
}

void EditBox::updateInfo(const fsb::Row &r) {
    m_lineLabel->setText(tr("Line: %1").arg(m_row + 1));
    m_wrapsLabel->setText(tr("Wraps: %1").arg(fs::q(r.wraps)));
    m_wrapsLabel->setStyleSheet(limits::wrapsOver(fs::q(r.wraps)) ? "color: #e05050" : "");
    const int cps = int(r.cps + 0.5);
    m_cpsLabel->setText(tr("Characters per second: %1<=%2").arg(cps).arg(limits::cps()));
    m_cpsLabel->setStyleSheet(cps > limits::cps() ? "color: #e05050" : "");
}

void EditBox::commit(const char *field) {
    if (m_loading || m_row < 0) return;
    auto &s = m_tab->s();
    fsb::Row r = s.row(size_t(m_row));
    const QString f(field);
    auto *v = m_tab->video();
    auto ms = [&](TimeEdit *t, bool end) {
        if (!m_framesMode) return t->ms();
        return t->frame() < 0 ? qint64(-1) : end ? v->timeAtEnd(t->frame()) : v->timeAtStart(t->frame());
    };
    const bool timeField = f == "start" || f == "end" || f == "duration";
    if (!timeField) m_initialTimes.clear();
    // Aegisub's CommitTimes: the typed time against the line's times from before the typing, so an
    // end pushed by a start typed past it comes back with the next digit
    const qint64 start = ms(m_start, false), end = ms(m_end, true);
    auto retime = [&](size_t row, std::int64_t &rs, std::int64_t &re) {
        if (!timeField) return;
        auto &[s0, e0] = m_initialTimes.try_emplace(row, rs, re).first->second;
        if (f == "start" && start >= 0) {
            s0 = rs = start;
            re = std::max(s0, e0);
        } else if (f == "end" && end >= 0) {
            e0 = re = end;
            rs = std::min(s0, e0);
        } else if (f == "duration") {
            // frames: the count from the first frame through the last (Aegisub)
            const qint64 e = !m_framesMode              ? (m_duration->ms() < 0 ? -1 : rs + m_duration->ms())
                             : m_duration->frame() < 0 ? -1
                                                       : v->timeAtEnd(v->frameAtStart(rs) + m_duration->frame() - 1);
            if (e >= rs) e0 = re = e;
        }
    };
    r.text = fs::rs(m_text->text().replace('\n', "\\N"));
    r.original = fs::rs(m_original->text().replace('\n', "\\N"));
    r.doubtful = m_doubtful->isChecked();
    r.comment = m_comment->isChecked();
    r.layer = m_layer->value();
    retime(size_t(m_row), r.start, r.end);
    r.style = fs::rs(m_style->currentText());
    r.actor = fs::rs(m_actor->currentText());
    r.effect = fs::rs(m_effect->currentText());
    r.margin_l = m_ml->value();
    r.margin_r = m_mr->value();
    r.margin_v = m_mv->value();
    if (m_lastEdit.isValid() && m_lastEdit.elapsed() > 30000) ++m_undoStep;
    m_lastEdit.start();
    const fs::Utf8 key(QString("edit:%1:%2:%3").arg(m_row).arg(f).arg(m_undoStep));
    s.set_row(size_t(m_row), r, key);
    // Aegisub's SetSelectedRows: the change goes to every selected line, in the same undo step
    const auto sel = m_tab->selection();
    const bool tl = s.file_info().tl_mode;
    if (sel.size() > 1 && f != "original" && f != "doubtful" && !(tl && f == "text")) {
        std::vector<size_t> rows;
        rust::Vec<rust::String> values;
        for (size_t row : sel) {
            if (row == size_t(m_row) || row >= s.row_count()) continue;
            rows.push_back(row);
            if (timeField) {
                fsb::Row o = s.row(row);
                retime(row, o.start, o.end);
                values.push_back(fs::rs(QString("%1,%2").arg(o.start).arg(o.end)));
            } else {
                const QString value = f == "comment" ? QString::number(int(r.comment))
                                      : f == "layer" ? QString::number(r.layer)
                                      : f == "margin_l" ? QString::number(r.margin_l)
                                      : f == "margin_r" ? QString::number(r.margin_r)
                                      : f == "margin_v" ? QString::number(r.margin_v)
                                                        : fs::q(f == "text"    ? r.text
                                                                : f == "style" ? r.style
                                                                : f == "actor" ? r.actor
                                                                               : r.effect);
                values.push_back(fs::rs(value));
            }
        }
        if (!rows.empty())
            s.set_rows_values(fs::slice(rows), fs::Utf8(timeField ? QString("times") : f), {values.data(), values.size()},
                              key);
    }
    m_tab->changed(TabPanel::FromEditBox);
    m_loading = true;
    const fsb::Row now = s.row(size_t(m_row));
    showTimes(now, f);
    updateInfo(now);
    m_loading = false;
}

// Frames as Aegisub shows them: the first and the last frame the line is on, and their count.
void EditBox::showTimes(const fsb::Row &r, const QString &skip) {
    auto *v = m_tab->video();
    const qint64 first = v->hasVideo() ? v->frameAtStart(r.start) : 0;
    const qint64 last = v->hasVideo() ? v->frameAtEnd(r.end) : 0;
    if (skip != "start") m_start->setMs(r.start, first);
    if (skip != "end") m_end->setMs(r.end, last);
    if (skip != "duration") m_duration->setMs(r.end - r.start, last - first + 1);
}

void EditBox::focusText() { m_text->setFocus(); }

void EditBox::selectText(int from, int length, bool original) {
    auto *e = original ? m_original : m_text;
    QTextCursor c(e->document());
    c.setPosition(from);
    c.setPosition(from + length, QTextCursor::KeepAnchor);
    e->setTextCursor(c);
}

std::pair<int, int> EditBox::textSelection(bool original) const {
    const auto c = (original ? m_original : m_text)->textCursor();
    return {c.selectionStart(), c.selectionEnd()};
}

void EditBox::scriptCursor(int64_t &cursor, int64_t &start, int64_t &end) const {
    const QTextCursor c = m_text->textCursor();
    const QString t = m_text->toPlainText();
    auto bytes = [&t](int pos) { return int64_t(t.left(pos).toUtf8().size()) + 1; };
    std::tie(cursor, start, end) = std::tuple(bytes(c.position()), bytes(c.selectionStart()), bytes(c.selectionEnd()));
}

void EditBox::setScriptCursor(int64_t cursor, int64_t start, int64_t end) {
    const QByteArray u = m_text->toPlainText().toUtf8();
    auto pos = [&u](int64_t b) { return int(QString::fromUtf8(u.left(qsizetype(std::clamp<int64_t>(b - 1, 0, u.size())))).size()); };
    const int caret = pos(cursor), a = pos(start), b = pos(end);
    QTextCursor c(m_text->document());
    c.setPosition(caret == a ? b : a); // the anchor at the selection's other end
    c.setPosition(caret, QTextCursor::KeepAnchor);
    m_text->setTextCursor(c);
}

QString EditBox::selectedText(bool original) const { return (original ? m_original : m_text)->textCursor().selectedText(); }

void EditBox::nextLine() {
    const auto vis = m_tab->grid()->visibleRows();
    auto it = std::upper_bound(vis.begin(), vis.end(), size_t(m_row));
    if (it != vis.end()) m_tab->setActive(int(*it));
    else {
        // Enter on the last line appends a new one, as in Kainote
        m_tab->grid()->insertRow(true, 0);
    }
    m_text->setFocus();
}

void EditBox::splitAtCursor(qint64 atMs, bool original) {
    SubsTextEdit *box = original ? m_original : m_text;
    const QString t = box->text();
    const auto r = m_tab->s().split_row(size_t(m_row), bytePos(t, box->textCursor().position()), atMs, original);
    m_tab->changed();
    if (r >= 0) m_tab->setActive(int(r));
}

// Kainote's edit box hotkeys and defaults, with Aegisub's colours and hard space
static const PanelKey kEditKeys[] = {
    {"editNextLine", QT_TRANSLATE_NOOP("EditBox", "Go to the next line"), "Return"},
    {"editLineBreak", QT_TRANSLATE_NOOP("EditBox", "Insert a line break"), "Shift+Return"},
    {"editHardSpace", QT_TRANSLATE_NOOP("EditBox", "Insert a hard space"), "Ctrl+Shift+Space"},
    {"editBold", QT_TRANSLATE_NOOP("EditBox", "Bold"), "Ctrl+B"},
    {"editItalic", QT_TRANSLATE_NOOP("EditBox", "Italic"), "Ctrl+I"},
    {"editUnderline", QT_TRANSLATE_NOOP("EditBox", "Underline"), ""},
    {"editStrikeout", QT_TRANSLATE_NOOP("EditBox", "Strikethrough"), ""},
    {"editColor1", QT_TRANSLATE_NOOP("EditBox", "Primary color"), "Alt+1"},
    {"editColor2", QT_TRANSLATE_NOOP("EditBox", "Secondary color for karaoke"), "Alt+2"},
    {"editColor3", QT_TRANSLATE_NOOP("EditBox", "Border color"), "Alt+3"},
    {"editColor4", QT_TRANSLATE_NOOP("EditBox", "Shadow color"), "Alt+4"},
    {"editStartDiff", QT_TRANSLATE_NOOP("EditBox", "Insert the video time's distance from the start"), "Ctrl+,"},
    {"editEndDiff", QT_TRANSLATE_NOOP("EditBox", "Insert the video time's distance to the end"), "Ctrl+."},
    {"editNextDoubtful", QT_TRANSLATE_NOOP("EditBox", "Next unconfirmed line"), "Ctrl+D"},
    {"editNextUntranslated", QT_TRANSLATE_NOOP("EditBox", "Next untranslated line"), "Ctrl+R"},
    {"editSetDoubtful", QT_TRANSLATE_NOOP("EditBox", "Mark as not confirmed and go to the next line"), "Alt+Down"},
    // Aegisub's audio/commit in the edit box
    {"editCommit", QT_TRANSLATE_NOOP("EditBox", "Apply changes and stay on the line"), "Ctrl+Return"},
    // Aegisub's edit/line/split/estimate|preserve|video, edit/revert and edit/insert_original
    {"editSplitEstimate", QT_TRANSLATE_NOOP("EditBox", "Split at cursor (estimate times)"), ""},
    {"editSplitPreserve", QT_TRANSLATE_NOOP("EditBox", "Split at cursor (preserve times)"), ""},
    {"editSplitVideo", QT_TRANSLATE_NOOP("EditBox", "Split at cursor (at video frame)"), ""},
    {"editRevert", QT_TRANSLATE_NOOP("EditBox", "Revert"), ""},
    {"editInsertOriginal", QT_TRANSLATE_NOOP("EditBox", "Insert original"), ""},
};

std::span<const PanelKey> EditBox::keys() { return kEditKeys; }

void EditBox::runCommand(const QString &id, SubsTextEdit *e) {
    if (m_row < 0) return;
    if (!e) e = m_text;
    auto &s = m_tab->s();
    const bool tl = s.file_info().tl_mode;
    if (id == "editNextLine") nextLine();
    else if (id == "editLineBreak") {
        // a line break in place of the selection, the spaces around it dropped (Kainote, Aegisub)
        const QString t = e->text();
        auto c = e->textCursor();
        int a = c.selectionStart(), b = c.selectionEnd();
        if (a > 0 && t[a - 1] == ' ') --a;
        if (b < t.size() && t[b] == ' ') ++b;
        c.setPosition(a);
        c.setPosition(b, QTextCursor::KeepAnchor);
        c.insertText(QSettings().value("edit/softBreak", false).toBool() ? "\\n" : "\\N"); // Aegisub's Soft Line Break
        e->setTextCursor(c);
    } else if (id == "editHardSpace") e->insertPlainText("\\h");
    else if (id == "editBold") toggleTag("b");
    else if (id == "editItalic") toggleTag("i");
    else if (id == "editUnderline") toggleTag("u");
    else if (id == "editStrikeout") toggleTag("s");
    else if (id.startsWith("editColor")) setColorTag(id.back().digitValue()); // Aegisub's edit/color/*
    else if (id == "editNextDoubtful" || id == "editNextUntranslated") {
        if (!tl) return;
        const bool doubtful = id == "editNextDoubtful";
        const auto r = doubtful ? s.next_doubtful(size_t(m_row)) : s.next_untranslated(size_t(m_row));
        if (r >= 0) m_tab->setActive(int(r));
        else m_tab->main()->status(doubtful ? tr("No unconfirmed lines found") : tr("No untranslated lines found"));
    } else if (id == "editStartDiff" || id == "editEndDiff") {
        // Kainote: the video time's distance from the line's start (end), only within the line
        const fsb::Row r = s.row(size_t(m_row));
        const qint64 t = m_tab->videoTime();
        if (!m_tab->video()->hasVideo() || t < r.start || t > r.end) return QApplication::beep();
        m_text->insertPlainText(QString::number(id == "editStartDiff" ? t - r.start : r.end - t));
    } else if (id == "editSetDoubtful" && tl) {
        const size_t row = size_t(m_row);
        s.set_doubtful({&row, 1}, true);
        m_tab->changed();
        nextLine();
    } else if (id == "editSplitEstimate" || id == "editSplitPreserve" || id == "editSplitVideo") {
        emit e->splitRequested(id == "editSplitEstimate" ? -1 : id == "editSplitPreserve" ? -2 : 0);
    } else if (id == "editRevert") {
        emit m_text->revertRequested();
    } else if (id == "editInsertOriginal") {
        emit m_text->insertOriginalRequested();
    } else if (id == "editCommit") {
        // the text is applied as it is typed: what follows is an undo step of its own, and the
        // audio's selection goes to the line
        ++m_undoStep;
        m_initialTimes.clear();
        if (m_tab->audio()->hasAudio()) m_tab->audio()->runCommand("audioCommitStay");
    }
}
