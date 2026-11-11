#include "historydialog.h"

#include "tabpanel.h"

#include <QCoreApplication>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

QString undoStepName(const QString &description) {
    // the core's undo steps, for the translation catalogue
    [[maybe_unused]] static const char *const steps[] = {
        QT_TRANSLATE_NOOP("UndoStep", "Add lines to tree"),
        QT_TRANSLATE_NOOP("UndoStep", "Add styles"),
        QT_TRANSLATE_NOOP("UndoStep", "Attach file"),
        QT_TRANSLATE_NOOP("UndoStep", "Attach fonts"),
        QT_TRANSLATE_NOOP("UndoStep", "Change frame rate"),
        QT_TRANSLATE_NOOP("UndoStep", "Change tags"),
        QT_TRANSLATE_NOOP("UndoStep", "Close tree"),
        QT_TRANSLATE_NOOP("UndoStep", "Continuous times"),
        QT_TRANSLATE_NOOP("UndoStep", "Convert"),
        QT_TRANSLATE_NOOP("UndoStep", "Correct minor errors"),
        QT_TRANSLATE_NOOP("UndoStep", "Delete attachment"),
        QT_TRANSLATE_NOOP("UndoStep", "Delete lines"),
        QT_TRANSLATE_NOOP("UndoStep", "Delete styles"),
        QT_TRANSLATE_NOOP("UndoStep", "Delete text"),
        QT_TRANSLATE_NOOP("UndoStep", "Delete unused styles"),
        QT_TRANSLATE_NOOP("UndoStep", "Duplicate lines"),
        QT_TRANSLATE_NOOP("UndoStep", "Edit line"),
        QT_TRANSLATE_NOOP("UndoStep", "Edit style"),
        QT_TRANSLATE_NOOP("UndoStep", "Edit styles"),
        QT_TRANSLATE_NOOP("UndoStep", "Insert line"),
        QT_TRANSLATE_NOOP("UndoStep", "Join lines"),
        QT_TRANSLATE_NOOP("UndoStep", "Kanji timing"),
        QT_TRANSLATE_NOOP("UndoStep", "Karaoke join"),
        QT_TRANSLATE_NOOP("UndoStep", "Karaoke split"),
        QT_TRANSLATE_NOOP("UndoStep", "Karaoke tag"),
        QT_TRANSLATE_NOOP("UndoStep", "Karaoke timing"),
        QT_TRANSLATE_NOOP("UndoStep", "Make tree"),
        QT_TRANSLATE_NOOP("UndoStep", "Move lines"),
        QT_TRANSLATE_NOOP("UndoStep", "Move styles"),
        QT_TRANSLATE_NOOP("UndoStep", "Move translation"),
        QT_TRANSLATE_NOOP("UndoStep", "Not confirmed"),
        QT_TRANSLATE_NOOP("UndoStep", "Open tree"),
        QT_TRANSLATE_NOOP("UndoStep", "Paste lines"),
        QT_TRANSLATE_NOOP("UndoStep", "Paste lines over"),
        QT_TRANSLATE_NOOP("UndoStep", "Paste translation"),
        QT_TRANSLATE_NOOP("UndoStep", "Post-process timing"),
        QT_TRANSLATE_NOOP("UndoStep", "Recombine lines"),
        QT_TRANSLATE_NOOP("UndoStep", "Remove tree"),
        QT_TRANSLATE_NOOP("UndoStep", "Replace"),
        QT_TRANSLATE_NOOP("UndoStep", "Resample resolution"),
        QT_TRANSLATE_NOOP("UndoStep", "Set FPS from video"),
        QT_TRANSLATE_NOOP("UndoStep", "Shift positions"),
        QT_TRANSLATE_NOOP("UndoStep", "Shift times"),
        QT_TRANSLATE_NOOP("UndoStep", "Snap to video"),
        QT_TRANSLATE_NOOP("UndoStep", "Sort lines"),
        QT_TRANSLATE_NOOP("UndoStep", "Spell check"),
        QT_TRANSLATE_NOOP("UndoStep", "Split by karaoke"),
        QT_TRANSLATE_NOOP("UndoStep", "Split line"),
        QT_TRANSLATE_NOOP("UndoStep", "Split lines"),
        QT_TRANSLATE_NOOP("UndoStep", "Swap lines"),
        QT_TRANSLATE_NOOP("UndoStep", "Translation assistant"),
        QT_TRANSLATE_NOOP("UndoStep", "Translation mode off"),
        QT_TRANSLATE_NOOP("UndoStep", "Translation mode on"),
        QT_TRANSLATE_NOOP("UndoStep", "Vector drawing"),
        QT_TRANSLATE_NOOP("UndoStep", "Visual typesetting"),
    };
    return QCoreApplication::translate("UndoStep", description.toUtf8().constData());
}

HistoryDialog::HistoryDialog(TabPanel *tab) : QDialog(tab) {
    setWindowTitle(tr("History"));
    auto *v = new QVBoxLayout(this);
    auto *list = new QListWidget(this);
    list->addItem(tr("(current state)"));
    for (const auto &h : tab->s().history()) list->addItem(undoStepName(fs::q(h)));
    v->addWidget(list);
    v->addWidget(new QLabel(tr("Click a step to undo back to before it."), this));
    connect(list, &QListWidget::itemClicked, this, [this, tab, list](QListWidgetItem *it) {
        const int steps = list->row(it);
        if (steps <= 0) return;
        tab->s().undo_to(size_t(steps));
        tab->reselect();
        accept();
    });
    resize(360, 420);
}
