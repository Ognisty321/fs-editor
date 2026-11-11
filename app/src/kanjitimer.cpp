#include "kanjitimer.h"

#include "tabpanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <vector>

KanjiTimerDialog::KanjiTimerDialog(QWidget *parent, TabPanel *tab)
    : QDialog(parent), m_tab(tab), m_kt(fsb::new_kanji_timer()) {
    setWindowTitle(tr("Kanji timing"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(true); // it writes to rows it noted, so the grid stays as it is (Aegisub)
    setFocusPolicy(Qt::StrongFocus);
    auto *v = new QVBoxLayout(this);
    auto *textBox = new QGroupBox(tr("Text"), this);
    auto *tv = new QVBoxLayout(textBox);
    m_srcView = new QLabel(textBox);
    m_dstView = new QLabel(textBox);
    for (auto *l : {m_srcView, m_dstView}) {
        l->setTextFormat(Qt::RichText);
        l->setMinimumHeight(36);
        QFont f = l->font();
        f.setPointSizeF(f.pointSizeF() * 1.6);
        l->setFont(f);
        tv->addWidget(l);
    }
    v->addWidget(textBox);

    auto *row = new QHBoxLayout;
    auto *styles = new QGroupBox(tr("Styles"), this);
    auto *sf = new QFormLayout(styles);
    m_src = new QComboBox(styles);
    m_dst = new QComboBox(styles);
    for (const auto &st : tab->s().styles()) m_src->addItem(fs::q(st.name)), m_dst->addItem(fs::q(st.name));
    QSettings set;
    m_src->setCurrentText(set.value("kanji/source").toString());
    m_dst->setCurrentText(set.value("kanji/dest").toString());
    m_interpolate = new QCheckBox(tr("Attempt to interpolate kanji"), styles);
    m_interpolate->setChecked(set.value("kanji/interpolate", true).toBool());
    sf->addRow(tr("Source:"), m_src);
    sf->addRow(tr("Dest:"), m_dst);
    sf->addRow(m_interpolate);
    row->addWidget(styles, 1);

    auto *cmds = new QGroupBox(tr("Commands"), this);
    auto *cv = new QVBoxLayout(cmds);
    auto button = [&](const QString &text, auto fn) {
        auto *b = new QPushButton(text, cmds);
        b->setAutoDefault(false);
        b->setFocusPolicy(Qt::NoFocus); // keys go to the dialog (Enter links, arrows resize)
        connect(b, &QPushButton::clicked, this, fn);
        cv->addWidget(b);
    };
    button(tr("Start"), [this] {
        if (m_src->currentText() == m_dst->currentText()) {
            QMessageBox::warning(this, windowTitle(), tr("The source and destination styles must be different."));
            return;
        }
        QSettings st;
        st.setValue("kanji/source", m_src->currentText());
        st.setValue("kanji/dest", m_dst->currentText());
        m_changes.clear();
        m_srcRow = find(-1, m_src->currentText(), 1);
        m_dstRow = find(-1, m_dst->currentText(), 1);
        reset();
    });
    button(tr("Link"), [this] { link(); });
    button(tr("Unlink"), [this] {
        if (!m_kt->undo()) QApplication::beep();
        refresh();
    });
    button(tr("Skip source line"), [this] {
        m_srcRow = find(m_srcRow, m_src->currentText(), 1);
        reset();
    });
    button(tr("Skip dest line"), [this] {
        m_dstRow = find(m_dstRow, m_dst->currentText(), 1);
        reset();
    });
    button(tr("Go back a line"), [this] {
        if (!m_changes.empty()) m_changes.pop_back();
        m_srcRow = find(m_srcRow, m_src->currentText(), -1);
        m_dstRow = find(m_dstRow, m_dst->currentText(), -1);
        reset();
    });
    button(tr("Accept line"), [this] { acceptLine(); });
    button(tr("Close"), [this] { close(); });
    row->addWidget(cmds);
    v->addLayout(row);
    auto *help = new QLabel(tr("Right / Left: longer / shorter destination selection\n"
                               "Up / Down: longer / shorter source selection\n"
                               "Enter: link, or accept the line when all is linked\n"
                               "Backspace: unlink the last group"),
                            this);
    help->setEnabled(false);
    v->addWidget(help);
    connect(m_interpolate, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("kanji/interpolate", on); });
    refresh();
    resize(720, sizeHint().height());
}

// The next (step 1) or previous (step -1) non-empty line in the style.
int KanjiTimerDialog::find(int from, const QString &style, int step) const {
    if (!m_tab || (from < 0 && step < 0)) return -1;
    const int n = int(m_tab->s().row_count());
    for (int i = from + step; i >= 0 && i < n; i += step) {
        const auto r = m_tab->s().row(size_t(i));
        if (fs::q(r.style) == style && !r.text.empty()) return i;
    }
    return -1;
}

void KanjiTimerDialog::reset() {
    if (m_srcRow < 0 || m_dstRow < 0) {
        m_srcRow = m_dstRow = -1;
        QApplication::beep();
        m_kt = fsb::new_kanji_timer();
    } else {
        m_kt->set_lines(m_tab->s(), size_t(m_srcRow), size_t(m_dstRow));
        if (m_interpolate->isChecked()) m_kt->auto_match();
    }
    setFocus(); // the arrows and Enter drive the matching
    refresh();
}

void KanjiTimerDialog::refresh() {
    const auto st = m_kt->state();
    auto span = [](const QString &t, const char *style) {
        return QString("<span style=\"%1\">%2</span>").arg(style, t.toHtmlEscaped().replace(' ', "&nbsp;"));
    };
    const char *done = "background:#3a3f4b;color:#9aa3b5", *sel = "background:#f08a5d;color:#101010",
               *sep = "color:#5a6070";
    QStringList src, dst;
    for (size_t k = 0; k < st.group_src.size(); ++k) {
        src << span(fs::q(st.group_src[k]), done);
        dst << span(fs::q(st.group_dst[k]), done);
    }
    QString cur;
    for (const auto &s : st.sel_src) cur += fs::q(s);
    if (!cur.isEmpty()) src << span(cur, sel);
    for (const auto &s : st.rest_src) src << span(fs::q(s), "");
    if (!st.sel_dst.empty()) dst << span(fs::q(st.sel_dst), sel);
    if (!st.rest_dst.empty()) dst << span(fs::q(st.rest_dst), "");
    const QString bar = span("|", sep);
    m_srcView->setText(src.join(bar));
    m_dstView->setText(dst.join(bar));
}

void KanjiTimerDialog::link() {
    if (m_kt->accept()) {
        if (m_interpolate->isChecked()) m_kt->auto_match();
    } else {
        QApplication::beep();
    }
    refresh();
}

void KanjiTimerDialog::acceptLine() {
    if (m_dstRow < 0) return;
    if (m_kt->state().remaining_src > 0) {
        QMessageBox::warning(this, windowTitle(), tr("Group all of the source text."));
        return;
    }
    m_changes.emplace_back(size_t(m_dstRow), fs::q(m_kt->output()));
    m_srcRow = find(m_srcRow, m_src->currentText(), 1);
    m_dstRow = find(m_dstRow, m_dst->currentText(), 1);
    reset();
}

void KanjiTimerDialog::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Backspace:
        if (!m_kt->undo()) QApplication::beep();
        break;
    case Qt::Key_Right: m_kt->adjust(1, true); break;
    case Qt::Key_Left: m_kt->adjust(1, false); break;
    case Qt::Key_Up: m_kt->adjust(0, true); break;
    case Qt::Key_Down: m_kt->adjust(0, false); break;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        const auto st = m_kt->state();
        if (st.remaining_src == 0 && st.remaining_dst == 0) acceptLine();
        else link();
        return;
    }
    default: QDialog::keyPressEvent(e); return;
    }
    refresh();
}

// Like Aegisub, the accepted lines are written when the dialog closes, as one undo step.
void KanjiTimerDialog::done(int r) {
    if (m_tab && !m_changes.empty()) {
        std::vector<size_t> rows;
        rust::Vec<rust::String> texts;
        for (const auto &[row, text] : m_changes) rows.push_back(row), texts.push_back(fs::rs(text));
        m_tab->s().set_texts(fs::slice(rows), {texts.data(), texts.size()}, "Kanji timing");
        m_tab->changed();
        m_changes.clear();
    }
    QDialog::done(r);
}
