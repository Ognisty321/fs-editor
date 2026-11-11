// MainWindow's Subtitles menu.
#include "mainwindow_p.h"

#include "assistants.h"
#include "attachmentsdialog.h"
#include "editbox.h"
#include "fontcollector.h"
#include "grid.h"
#include "kanjitimer.h"
#include "propertiesdialog.h"
#include "resampledialog.h"
#include "shiftpanel.h"
#include "spellcheckdialog.h"
#include "stylemanager.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>

#include <iterator>
#include <tuple>
#include <utility>

namespace {
// Format badge (ASS, SRT, ...) for the conversion actions, as Kainote shows them.
QIcon glyph(const QString &text) {
    QPixmap px(32, 32);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kBlue, 2));
    QFont f = QApplication::font();
    f.setBold(true);
    f.setPixelSize(text.size() > 2 ? 11 : 14);
    p.setFont(f);
    p.drawRoundedRect(QRectF(1.5, 1.5, 29, 29), 5, 5);
    p.drawText(px.rect(), Qt::AlignCenter, text);
    return QIcon(px);
}
} // namespace

void MainWindow::buildSubtitlesMenu(Menus &menus) {
    auto tab = [this] { return current(); };

    auto *subs = menus.subs = barMenu(tr("&Subtitles"));
    auto *editor = add("editor", tr("Enable / Disable editor"), QKeySequence("Ctrl+E"), subs, {});
    editor->setCheckable(true);
    editor->setChecked(true);
    connect(editor, &QAction::triggered, this, [=](bool on) { tab()->setPlayerMode(!on); });
    add("properties", tr("ASS file properties"), {}, subs, [=] { PropertiesDialog(tab()).exec(); });
    add("styleManager", tr("Style manager"), QKeySequence("Ctrl+M"), subs, [this] {
        if (!m_styles) {
            m_styles = new StyleManager(this);
            m_styles->setAttribute(Qt::WA_DeleteOnClose);
        }
        m_styles->show();
        m_styles->raise();
        m_styles->activateWindow();
    });
    // Kainote's clean styles of the file, as the style manager's button does it
    add("cleanStyles", tr("Delete unused styles"), {}, subs, [=, this] {
        QStringList gone;
        for (const auto &n : tab()->s().clean_styles()) gone << fs::q(n);
        tab()->changed();
        QMessageBox::information(this, tr("Delete unused styles"),
                                 gone.isEmpty() ? tr("Every style is in use.") : tr("Deleted styles:\n%1").arg(gone.join("\n")));
    });
    auto *conv = subMenu(subs, tr("Conversion"));
    const std::tuple<const char *, const char *, const char *> formats[] = {
        {"toASS", "ASS", "F9"}, {"toSRT", "SRT", "F8"}, {"toMDVD", "MicroDVD", "F10"},
        {"toMPL2", "MPL2", "F11"}, {"toTMP", "TMP", "Ctrl+F12"}};
    for (auto [id, fmt, key] : formats)
        add(id, tr("Convert to %1").arg(fmt == QString("MicroDVD") ? "MDVD" : fmt), QKeySequence(key), conv,
            [this, fmt] { convert(fmt); })
            ->setIcon(glyph(QString(fmt).left(3)));
    auto *shift = add("shiftTimes", tr("Shift times"), QKeySequence("Ctrl+I"), subs, {});
    shift->setCheckable(true);
    shift->setChecked(QSettings().value("layout/shiftTimes", true).toBool());
    connect(shift, &QAction::triggered, this, [this](bool on) {
        for (auto *t : tabs()) t->showShiftPanel(on);
    });
    // Kainote's: runs the panel's page shown, hidden or not
    add("runShift", tr("Shift times / run post processor"), {}, subs, [=] { tab()->shift()->run(); });
    // Kainote's: two lines selected, the first keeps its start and the second's moves to the
    // video; every line stretches with them
    add("fpsFromVideo", tr("Set FPS from video"), {}, subs, [=, this] {
        auto *t = tab();
        const auto sel = t->selection();
        if (sel.size() != 2 || !t->video()->hasVideo()) return status(tr("Select two lines and open a video"));
        t->s().stretch_times(t->s().row(sel[0]).start, t->s().row(sel[1]).start, t->videoStartTime());
        t->changed();
    });
    // Kainote "Set new FPS" / Aegisub "Transform Framerate": keep every line on its frames
    add("changeFps", tr("Change frame rate..."), {}, subs, [=, this] {
        auto *t = tab();
        const bool video = t->video()->hasVideo();
        QDialog d(this);
        d.setWindowTitle(tr("Change frame rate"));
        auto *f = new QFormLayout(&d);
        const QStringList rates = {"23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60"};
        auto rate = [&](const QString &def) {
            auto *c = new QComboBox(&d);
            c->setEditable(true);
            if (video) c->addItem(tr("From video"), "video");
            c->addItems(rates);
            c->setCurrentText(def);
            return c;
        };
        auto *from = rate(video ? tr("From video") : "23.976");
        auto *to = rate("25");
        f->addRow(tr("Subtitles timed to:"), from);
        f->addRow(tr("Convert to:"), to);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addRow(bb);
        if (d.exec() != QDialog::Accepted) return;
        auto value = [](QComboBox *c) {
            return c->currentText() == c->itemText(0) && c->itemData(0).isValid() ? QString("video") : c->currentText();
        };
        const auto tc = t->video()->timecodes();
        try {
            t->s().change_fps(fs::Utf8(value(from)), fs::Utf8(value(to)), {tc.data(), tc.size()});
            t->changed();
        } catch (const rust::Error &e) {
            error(QString::fromUtf8(e.what()));
        }
    });
    add("fontCollector", tr("Fonts collector"), {}, subs, [=, this] { FontCollectorDialog(tab()).exec(); });
    add("resample", tr("Resample subtitles"), {}, subs, [=] { ResampleDialog(tab()).exec(); });
    // Aegisub time/snap/scene: selected lines span the keyframes around the video frame
    add("snapScene", tr("Snap to scene"), {}, subs, [=] {
        auto *t = tab();
        const auto kf = t->keyframes();
        auto *v = t->video();
        if (kf.empty() || !v->hasVideo()) return;
        const qint64 cur = v->frame();
        qint64 prev = 0, next = v->info().frames;
        if (cur < kf.front()) next = kf.front();
        else if (cur < kf.back()) {
            auto it = std::upper_bound(kf.begin(), kf.end(), cur);
            prev = *std::prev(it);
            next = *it;
        } else prev = kf.back();
        t->s().set_rows_field(fs::slice(t->selection()), "times",
                              fs::Utf8(QString("%1,%2").arg(v->timeAtStart(prev)).arg(v->timeAtEnd(next - 1))));
        t->changed();
    });
    // Aegisub time/frame/current: shift the selection so the active line starts at the video frame
    add("shiftToFrame", tr("Shift to current frame"), {}, subs, [=] {
        auto *t = tab();
        if (t->active() < 0 || !t->video()->hasVideo()) return;
        fsb::ShiftSpec spec;
        spec.amount = t->videoStartTime() - t->s().row(size_t(t->active())).start;
        spec.lines = 1;
        for (size_t r : t->selection()) spec.selected.push_back(r);
        spec.active = size_t(t->active());
        const auto tc = t->video()->timecodes();
        t->s().shift(spec, {tc.data(), tc.size()});
        t->changed();
    });
    add("attachments", tr("Attachments..."), {}, subs, [=, this] { AttachmentsDialog(this, tab()).exec(); });
    add("kanjiTimer", tr("Kanji timer..."), {}, subs, [=, this] {
        if (tab()->s().row_count()) (new KanjiTimerDialog(this, tab()))->show();
    });
    add("stylingAssistant", tr("Styling assistant..."), {}, subs, [=, this] {
        if (auto *d = findChild<StylingAssistant *>()) return d->raise(), d->activateWindow();
        if (tab()->s().row_count()) (new StylingAssistant(this, tab()))->show();
    });
    addPanelKeys(subMenu(subs, tr("Styling assistant keys")), StylingAssistant::keys(), "StylingAssistant",
              [=, this](const char *id) {
                  if (auto *d = findChild<StylingAssistant *>()) d->runCommand(id);
              });
    // Aegisub's tool/translation_assistant, one at a time; its keys run in it while it is open
    add("translationAssistant", tr("Translation assistant..."), {}, subs, [=, this] {
        if (auto *d = findChild<TranslationAssistant *>()) return d->raise(), d->activateWindow();
        auto *d = new TranslationAssistant(this, tab());
        if (d->start()) d->show();
    });
    addPanelKeys(subMenu(subs, tr("Translation assistant keys")), TranslationAssistant::keys(), "TranslationAssistant",
              [=, this](const char *id) {
                  if (auto *d = findChild<TranslationAssistant *>()) d->runCommand(id);
              });
    // Kainote's window for lining a pasted translation up with the timed original; non-modal,
    // it acts on the grid selection
    add("translationMatching", tr("Translation matching..."), {}, subs, [this] {
        if (auto *open = findChild<QDialog *>("translationMatching")) return open->activateWindow();
        auto *d = new QDialog(this);
        d->setObjectName("translationMatching");
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle(tr("Translation matching"));
        auto *g = new QGridLayout(d);
        g->addWidget(new QLabel(tr("Original text"), d), 0, 0);
        g->addWidget(new QLabel(tr("Translation text"), d), 0, 1);
        const std::pair<const char *, const char *> buttons[] = {
            {QT_TR_NOOP("Insert line"),
             QT_TR_NOOP("Inserts an empty line before the selected one.\nThe original moves down.\n"
                        "Set the times of the inserted line.")},
            {QT_TR_NOOP("Insert line"),
             QT_TR_NOOP("Inserts an empty translation at the selected line.\nThe translation moves down.")},
            {QT_TR_NOOP("Join lines"), QT_TR_NOOP("Joins the next line with the selected one.\nThe original moves up.")},
            {QT_TR_NOOP("Join lines"),
             QT_TR_NOOP("Joins the next translation with the selected one.\nThe translation moves up.")},
            {QT_TR_NOOP("Delete line"), QT_TR_NOOP("Deletes the selected line.\nThe original moves up.")},
            {QT_TR_NOOP("Delete line"),
             QT_TR_NOOP("Deletes the translation of the selected line.\nThe translation moves up.")}};
        const uint8_t ops[] = {4, 1, 5, 2, 3, 0}; // fsb::tl_shift order
        for (int k = 0; k < 6; ++k) {
            auto *b = new QPushButton(tr(buttons[k].first), d);
            b->setToolTip(tr(buttons[k].second));
            g->addWidget(b, 1 + k / 2, k % 2);
            connect(b, &QPushButton::clicked, this, [this, op = ops[k]] {
                auto *t = current();
                const auto sel = t->selection();
                if (sel.empty() || !t->s().file_info().tl_mode) return;
                t->s().tl_shift(sel.front(), sel.size(), op);
                t->changed();
            });
        }
        auto *note = new QLabel(tr("Original: the timed subtitles the pasted dialogue is matched against, "
                                   "removed afterwards.\nTranslation: the pasted text, which takes the original's "
                                   "timing."),
                                d);
        note->setWordWrap(true);
        g->addWidget(note, 4, 0, 1, 2);
        d->show();
    });
    add("spellChecker", tr("Spell checker..."), {}, subs, [=, this] {
        auto *d = new SpellCheckDialog(this, tab());
        if (d->findNext()) d->show();
    });
    auto *spellOn = add("spellCheck", tr("Underline misspelled words"), {}, subs, {});
    spellOn->setCheckable(true);
    spellOn->setChecked(QSettings().value("spell/enabled", true).toBool());
    connect(spellOn, &QAction::triggered, this, [spellOn] {
        // from the setting: the edit box's language menu turns it off too
        const bool on = !QSettings().value("spell/enabled", true).toBool();
        spellOn->setChecked(on);
        QSettings().setValue("spell/enabled", on);
        spell::reload();
    });
    connect(subs, &QMenu::aboutToShow, spellOn,
            [spellOn] { spellOn->setChecked(QSettings().value("spell/enabled", true).toBool()); });
    // Aegisub's tag hiding modes in the grid; the toolbar's "hideTags" cycles them
    auto *tagModes = new QActionGroup(this);
    const std::pair<const char *, QString> tagNames[] = {
        {"tagsShow", tr("Show tags")}, {"tagsSimplify", tr("Simplify tags")}, {"tagsHide", tr("Hide tags")}};
    for (int k = 0; k < 3; ++k) {
        auto *a = add(tagNames[k].first, tagNames[k].second, {}, subs, [this, k] {
            QSettings().setValue("grid/tagMode", k);
            for (auto *t : tabs()) t->grid()->setTagMode(k);
        });
        a->setCheckable(true);
        a->setChecked(k == QSettings().value("grid/tagMode").toInt());
        tagModes->addAction(a);
    }
    add("hideTags", tr("Cycle tag hiding mode"), {}, subs, [this, tagModes] {
        const auto modes = tagModes->actions();
        QAction *next = modes[(modes.indexOf(tagModes->checkedAction()) + 1) % modes.size()];
        next->trigger();
        status(next->text());
    });
}
