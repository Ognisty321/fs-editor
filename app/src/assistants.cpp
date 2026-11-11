#include "assistants.h"

#include "audiobox.h"
#include "editbox.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QAction>
#include <QCheckBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTextCursor>
#include <QVBoxLayout>

#include <algorithm>
#include <span>
#include <vector>

static const PanelKey kTranslationKeys[] = {
    {"tlCommit", QT_TRANSLATE_NOOP("TranslationAssistant", "Accept changes"), "Return"},
    {"tlPreview", QT_TRANSLATE_NOOP("TranslationAssistant", "Preview changes"), "F8"},
    {"tlPrev", QT_TRANSLATE_NOOP("TranslationAssistant", "Previous line"), "PgUp"},
    {"tlNext", QT_TRANSLATE_NOOP("TranslationAssistant", "Next line"), "PgDown"},
    {"tlInsertOriginal", QT_TRANSLATE_NOOP("TranslationAssistant", "Insert original"), "Ins"},
    {"tlPlayVideo", QT_TRANSLATE_NOOP("TranslationAssistant", "Play video"), "Home"},
    {"tlPlayAudio", QT_TRANSLATE_NOOP("TranslationAssistant", "Play audio"), "End"},
};

std::span<const PanelKey> TranslationAssistant::keys() { return kTranslationKeys; }

TranslationAssistant::TranslationAssistant(MainWindow *main, TabPanel *tab) : QDialog(main), m_main(main), m_tab(tab) {
    setWindowTitle(tr("Translation assistant"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *v = new QVBoxLayout(this);
    auto *original = new QGroupBox(tr("Original"), this);
    auto *ov = new QVBoxLayout(original);
    m_lineNo = new QLabel(original);
    m_original = new QPlainTextEdit(original);
    m_original->setReadOnly(true);
    ov->addWidget(m_lineNo);
    ov->addWidget(m_original);
    auto *translated = new QGroupBox(tr("Translation"), this);
    auto *tv = new QVBoxLayout(translated);
    m_translation = new SubsTextEdit(translated);
    m_translation->installEventFilter(this);
    tv->addWidget(m_translation);
    v->addWidget(original, 1);
    v->addWidget(translated, 1);

    auto *h = new QHBoxLayout;
    auto *keysBox = new QGroupBox(tr("Keys"), this);
    auto *kv = new QFormLayout(keysBox);
    m_preview = new QCheckBox(tr("Enable preview"), keysBox);
    m_preview->setChecked(QSettings().value("translation/preview", true).toBool());
    connect(m_preview, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("translation/preview", on); });
    // the keys as bound now, with the window's Delete lines (Aegisub lists its edit/line/delete)
    const auto keyOf = [main](const char *id) {
        const QAction *a = main->action(id);
        return a ? QKeySequence::listToString(a->shortcuts(), QKeySequence::NativeText) : QString();
    };
    for (const auto &k : kTranslationKeys)
        kv->addRow(QCoreApplication::translate("TranslationAssistant", k.name), new QLabel(keyOf(k.id), keysBox));
    kv->addRow(tr("Delete line"), new QLabel(keyOf("deleteLines"), keysBox));
    kv->addRow(m_preview);
    h->addWidget(keysBox, 1);
    auto *actions = new QGroupBox(tr("Actions"), this);
    auto *av = new QVBoxLayout(actions);
    auto *playAudio = new QPushButton(tr("Play audio"), actions), *playVideo = new QPushButton(tr("Play video"), actions);
    playAudio->setEnabled(tab->audio()->hasAudio());
    playVideo->setEnabled(tab->video()->hasVideo());
    for (auto *b : {playAudio, playVideo}) {
        b->setAutoDefault(false);
        av->addWidget(b);
    }
    av->addStretch();
    h->addWidget(actions);
    v->addLayout(h);
    auto *close = new QPushButton(tr("Close"), this);
    close->setAutoDefault(false);
    v->addWidget(close, 0, Qt::AlignRight);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(playAudio, &QPushButton::clicked, this, [this] { runCommand("tlPlayAudio"); });
    connect(playVideo, &QPushButton::clicked, this, [this] { runCommand("tlPlayVideo"); });
    resize(480, 420);

    connect(tab, &TabPanel::activeChanged, this, [this] {
        if (!m_busy) lineChanged();
    });
    connect(tab, &TabPanel::docChanged, this, [this] {
        if (!m_busy) lineChanged(); // Aegisub starts the line over when its text changes elsewhere
    });
}

std::vector<uint32_t> TranslationAssistant::blocks() const {
    const auto b = fsb::translatable_blocks(m_tab->s().row(size_t(m_row)).text);
    return {b.begin(), b.end()};
}

bool TranslationAssistant::start() {
    m_row = std::max(0, m_tab->active());
    m_block = 0;
    if ((m_tab->s().row_count() && !blocks().empty()) || step(1)) {
        display();
        return true;
    }
    QMessageBox::information(m_main, windowTitle(), tr("There is nothing to translate in the file."));
    close();
    return false;
}

bool TranslationAssistant::step(int dir) {
    const int n = int(m_tab->s().row_count());
    if (const int count = int(blocks().size() / 2); m_block + dir >= 0 && m_block + dir < count) {
        m_block += dir;
        return true;
    }
    for (int r = m_row + dir; r >= 0 && r < n; r += dir) {
        const auto b = fsb::translatable_blocks(m_tab->s().row(size_t(r)).text);
        if (b.empty()) continue;
        m_row = r;
        m_block = dir > 0 ? 0 : int(b.size() / 2) - 1;
        m_busy = true;
        m_tab->setActive(r);
        m_busy = false;
        return true;
    }
    return false;
}

void TranslationAssistant::lineChanged() {
    if (!m_tab || m_tab->active() < 0) return;
    m_row = m_tab->active();
    m_block = 0;
    if (!blocks().empty() || step(1)) return display();
    QMessageBox::information(this, windowTitle(), tr("No more lines to translate."));
    close();
}

void TranslationAssistant::display() {
    const auto b = blocks();
    const QString text = fs::q(m_tab->s().row(size_t(m_row)).text);
    m_lineNo->setText(tr("Current line: %1/%2").arg(m_row + 1).arg(m_tab->s().row_count()));
    m_original->setCurrentCharFormat({}); // the last highlight would carry over to the whole text
    m_original->setPlainText(text);
    if (size_t(m_block) * 2 + 1 < b.size()) {
        QTextCursor c(m_original->document());
        c.setPosition(int(b[m_block * 2]));
        c.setPosition(int(b[m_block * 2] + b[m_block * 2 + 1]), QTextCursor::KeepAnchor);
        QTextCharFormat f;
        f.setFontWeight(QFont::Bold);
        f.setForeground(palette().color(QPalette::Link));
        c.mergeCharFormat(f);
        c.clearSelection();
        m_original->setTextCursor(c);
        m_original->ensureCursorVisible();
    }
    if (m_preview->isChecked()) m_tab->seekVideo(m_tab->s().row(size_t(m_row)).start);
    m_translation->setText({});
    m_translation->setFocus();
}

void TranslationAssistant::commit(bool next) {
    const auto b = blocks();
    if (size_t(m_block) * 2 + 1 >= b.size()) return lineChanged();
    QString value = m_translation->text();
    value.replace("\r\n", "\\N").replace('\r', "\\N").replace('\n', "\\N");
    QString text = fs::q(m_tab->s().row(size_t(m_row)).text);
    text.replace(int(b[m_block * 2]), int(b[m_block * 2 + 1]), value);
    const std::vector<size_t> rows{size_t(m_row)};
    const auto t = fs::rvec({text});
    m_busy = true;
    m_tab->s().set_texts(fs::slice(rows), {t.data(), t.size()}, "Translation assistant");
    m_tab->changed();
    m_busy = false;
    if (!next) {
        if (blocks().empty()) return lineChanged(); // the block went with the text typed in its place
        m_block = std::min(m_block, int(blocks().size() / 2) - 1);
        return display();
    }
    // the block replaced may now be none or several: go on after what was typed
    const auto now = blocks();
    const uint32_t end = b[m_block * 2] + uint32_t(value.size());
    m_block = -1;
    for (size_t k = 0; k + 1 < now.size(); k += 2)
        if (now[k] + now[k + 1] <= end) m_block = int(k / 2);
    if (step(1)) return display();
    QMessageBox::information(this, windowTitle(), tr("No more lines to translate."));
    close();
}

void TranslationAssistant::runCommand(const QString &id) {
    if (!m_tab) {
        close();
        return;
    }
    if (id == "tlCommit") commit(true);
    else if (id == "tlPreview") commit(false);
    else if (id == "tlNext" || id == "tlPrev") {
        if (step(id == "tlNext" ? 1 : -1)) display();
    } else if (id == "tlInsertOriginal") {
        const auto b = blocks();
        if (size_t(m_block) * 2 + 1 < b.size())
            m_translation->insertPlainText(fs::q(m_tab->s().row(size_t(m_row)).text).mid(int(b[m_block * 2]), int(b[m_block * 2 + 1])));
    } else if (id == "tlPlayVideo") {
        m_tab->playLine(m_row);
    } else if (id == "tlPlayAudio") {
        const fsb::Row r = m_tab->s().row(size_t(m_row));
        m_tab->audio()->play(r.start, r.end);
    }
    if (isVisible()) m_translation->setFocus();
}

bool TranslationAssistant::eventFilter(QObject *o, QEvent *e) {
    if (o == m_translation && (e->type() == QEvent::KeyPress || e->type() == QEvent::ShortcutOverride)) {
        auto *ke = static_cast<QKeyEvent *>(e);
        const char *id = m_main->panelCommand(ke, keys());
        // Shift+Enter breaks the line, as \N when committed (Aegisub)
        const bool lineBreak = !id && (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
                               (ke->modifiers() & Qt::ShiftModifier);
        if (id || lineBreak) {
            e->accept();
            if (e->type() == QEvent::KeyPress) {
                if (id) runCommand(id);
                else m_translation->insertPlainText("\\N");
            }
            return true;
        }
    }
    return QDialog::eventFilter(o, e);
}

static const PanelKey kStylingKeys[] = {
    {"styCommit", QT_TRANSLATE_NOOP("StylingAssistant", "Accept changes"), "Return"},
    {"styPreview", QT_TRANSLATE_NOOP("StylingAssistant", "Preview changes"), "F8"},
    {"styPrev", QT_TRANSLATE_NOOP("StylingAssistant", "Previous line"), "PgUp"},
    {"styNext", QT_TRANSLATE_NOOP("StylingAssistant", "Next line"), "PgDown"},
    {"styPlayVideo", QT_TRANSLATE_NOOP("StylingAssistant", "Play video"), "Home"},
    {"styPlayAudio", QT_TRANSLATE_NOOP("StylingAssistant", "Play audio"), "End"},
};

std::span<const PanelKey> StylingAssistant::keys() { return kStylingKeys; }

StylingAssistant::StylingAssistant(MainWindow *main, TabPanel *tab) : QDialog(main), m_main(main), m_tab(tab) {
    setWindowTitle(tr("Styling assistant"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *g = new QGridLayout(this);
    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setFixedHeight(64);
    m_styles = new QListWidget(this);
    QStringList names;
    for (const auto &s : tab->s().styles()) names << fs::q(s.name);
    m_styles->addItems(names);
    m_name = new QLineEdit(this);
    // Aegisub completes the typed prefix to the first matching style
    auto *c = new QCompleter(names, m_name);
    c->setCaseSensitivity(Qt::CaseInsensitive);
    c->setCompletionMode(QCompleter::InlineCompletion);
    m_name->setCompleter(c);
    m_seek = new QCheckBox(tr("Seek video to line start time"), this);
    m_seek->setChecked(QSettings().value("styling/seek", true).toBool());
    auto *playAudio = new QPushButton(tr("Play audio"), this), *playVideo = new QPushButton(tr("Play video"), this);
    for (auto *b : {playAudio, playVideo}) b->setAutoDefault(false);
    playAudio->setEnabled(tab->audio()->hasAudio());
    playVideo->setEnabled(tab->video()->hasVideo());
    // Aegisub's Keys box: the keys as bound now
    auto *keysBox = new QGroupBox(tr("Keys"), this);
    auto *kv = new QFormLayout(keysBox);
    for (const auto &k : kStylingKeys) {
        const QAction *a = main->action(k.id);
        kv->addRow(QCoreApplication::translate("StylingAssistant", k.name),
                   new QLabel(a ? QKeySequence::listToString(a->shortcuts(), QKeySequence::NativeText) : QString(), keysBox));
    }
    kv->addRow(tr("Click on list"), new QLabel(tr("Select style"), keysBox));
    kv->addRow(m_seek);
    g->addWidget(new QLabel(tr("Current line"), this), 0, 0, 1, 2);
    g->addWidget(m_text, 1, 0, 1, 2);
    g->addWidget(new QLabel(tr("Styles available"), this), 2, 0);
    g->addWidget(m_styles, 3, 0, 4, 1);
    g->addWidget(new QLabel(tr("Set style"), this), 2, 1);
    g->addWidget(m_name, 3, 1);
    g->addWidget(keysBox, 4, 1);
    auto *h = new QHBoxLayout;
    h->addWidget(playAudio);
    h->addWidget(playVideo);
    g->addLayout(h, 5, 1);
    g->setRowStretch(6, 1);
    m_name->installEventFilter(this);

    connect(m_name, &QLineEdit::textChanged, this, [this](const QString &t) {
        const bool known = !m_styles->findItems(t, Qt::MatchFixedString).isEmpty();
        m_name->setStyleSheet(t.isEmpty() || known ? QString() : "background: #803030");
    });
    connect(m_styles, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        m_name->setText(it->text());
        commit(false);
    });
    connect(m_styles, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        m_name->setText(it->text());
        commit(true);
    });
    connect(m_seek, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("styling/seek", on); });
    connect(playAudio, &QPushButton::clicked, this, [this] { runCommand("styPlayAudio"); });
    connect(playVideo, &QPushButton::clicked, this, [this] { runCommand("styPlayVideo"); });
    connect(tab, &TabPanel::activeChanged, this, &StylingAssistant::lineChanged);
    lineChanged();
}

void StylingAssistant::runCommand(const QString &id) {
    if (!m_tab || m_tab->active() < 0) {
        close();
        return;
    }
    const int row = m_tab->active();
    if (id == "styCommit") commit(true);
    else if (id == "styPreview") commit(false);
    else if (id == "styPrev" && row > 0) m_tab->setActive(row - 1);
    else if (id == "styNext" && size_t(row + 1) < m_tab->s().row_count()) m_tab->setActive(row + 1);
    else if (id == "styPlayVideo") m_tab->playLine(row);
    else if (id == "styPlayAudio") {
        const fsb::Row r = m_tab->s().row(size_t(row));
        m_tab->audio()->play(r.start, r.end);
    }
    if (isVisible()) m_name->setFocus();
}

bool StylingAssistant::eventFilter(QObject *o, QEvent *e) {
    if (o == m_name && (e->type() == QEvent::KeyPress || e->type() == QEvent::ShortcutOverride)) {
        if (const char *id = m_main->panelCommand(static_cast<QKeyEvent *>(e), keys())) {
            e->accept();
            if (e->type() == QEvent::KeyPress) runCommand(id);
            return true;
        }
    }
    return QDialog::eventFilter(o, e);
}

void StylingAssistant::lineChanged() {
    if (!m_tab || m_tab->active() < 0) return;
    const fsb::Row r = m_tab->s().row(size_t(m_tab->active()));
    m_text->setPlainText(fs::q(r.text));
    m_name->setText(fs::q(r.style));
    m_name->selectAll();
    m_name->setFocus();
    for (auto *it : m_styles->findItems(fs::q(r.style), Qt::MatchFixedString)) m_styles->setCurrentItem(it);
    if (m_seek->isChecked()) m_tab->seekVideo(r.start);
}

void StylingAssistant::commit(bool next) {
    if (!m_tab || m_tab->active() < 0) return;
    const auto found = m_styles->findItems(m_name->text(), Qt::MatchFixedString);
    if (found.isEmpty()) return; // Aegisub: only existing styles
    const std::vector<size_t> row{size_t(m_tab->active())};
    m_tab->s().set_rows_field(fs::slice(row), "style", fs::Utf8(found[0]->text()));
    m_tab->changed();
    if (next && size_t(m_tab->active() + 1) < m_tab->s().row_count()) m_tab->setActive(m_tab->active() + 1);
    else lineChanged();
}
