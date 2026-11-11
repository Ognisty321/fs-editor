// MainWindow's keys: a command's keys shown and changed, the hotkey window, and the event
// filter that gives a panel its own keys before the window's shortcuts take them.
#include "mainwindow_p.h"

#include "audiobox.h"
#include "editbox.h"
#include "grid.h"
#include "icons.h"
#include "tabpanel.h"
#include "videobox.h"
#include "visualtools.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>
#include <span>
#include <utility>
#include <vector>

// the command's own name: a menu text may say more (Undo's step); a macro's is kept apart
QString commandName(const QAction *a) {
    const QVariant name = a->property("name");
    return name.isValid() ? name.toString() : fs::plainText(a->text());
}

void showHotkey(QAction *a) {
    const QString key = QKeySequence::listToString(a->shortcuts(), QKeySequence::NativeText);
    a->setToolTip(key.isEmpty() ? commandName(a) : QString("%1 (%2)").arg(commandName(a), key));
}

namespace {
QList<QKeySequence> defaultKeys(const QAction *a) {
    const auto k = a->property("defaultKey").value<QKeySequence>();
    return k.isEmpty() ? QList<QKeySequence>{} : QList{k};
}
} // namespace

void MainWindow::assignHotkey(QAction *a) {
    QDialog d(this);
    d.setWindowTitle(tr("Hotkey for \"%1\"").arg(commandName(a)));
    auto *v = new QVBoxLayout(&d);
    v->addWidget(new QLabel(tr("Each of these keys runs the command:"), &d));
    auto *rows = new QVBoxLayout;
    v->addLayout(rows);
    std::vector<QKeySequenceEdit *> edits;
    std::function<void(const QKeySequence &)> addEdit = [&](const QKeySequence &k) {
        auto *e = new QKeySequenceEdit(k, &d);
        e->setMaximumSequenceLength(1);
        e->setClearButtonEnabled(true);
        rows->addWidget(e);
        edits.push_back(e);
        // a free row stays last, for one more key
        connect(e, &QKeySequenceEdit::keySequenceChanged, &d, [&, e](const QKeySequence &s) {
            if (!s.isEmpty() && e == edits.back()) addEdit({});
        });
    };
    for (const auto &k : a->shortcuts()) addEdit(k);
    addEdit({});
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset, &d);
    bb->button(QDialogButtonBox::Reset)->setText(tr("Clear"));
    connect(bb->button(QDialogButtonBox::Reset), &QPushButton::clicked, &d, [&] {
        for (auto *e : edits) e->clear();
    });
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    edits.front()->setFocus(); // a typed key replaces the first, as with one key per command
    const bool accepted = d.exec() == QDialog::Accepted;
    // the dialog's end takes the edits' focus, which signals into the locals gone by then
    for (auto *e : edits) disconnect(e, nullptr, &d, nullptr);
    if (!accepted) return;
    QList<QKeySequence> keys;
    for (auto *e : edits)
        if (!e->keySequence().isEmpty() && !keys.contains(e->keySequence())) keys << e->keySequence();
    setHotkey(a, keys);
}

// Takes each key over from another command after asking (that one keeps its other keys); default
// keys are not stored, so a new default applies.
bool MainWindow::setHotkey(QAction *a, const QList<QKeySequence> &keys) {
    const auto store = [](QAction *x) {
        QSettings st;
        QStringList l;
        for (const auto &k : x->shortcuts()) l << k.toString();
        if (x->shortcuts() == defaultKeys(x)) st.remove("hotkeys/" + x->objectName());
        else if (l.isEmpty()) st.setValue("hotkeys/" + x->objectName(), QString()); // an empty list reads back as unset
        else st.setValue("hotkeys/" + x->objectName(), l);
        showHotkey(x);
    };
    for (const auto &key : keys)
        for (auto *other : std::as_const(m_actions)) {
            // the audio's keys and the window's are apart
            if (other == a || !other->shortcuts().contains(key) || other->property("context") != a->property("context"))
                continue;
            if (!confirm(tr("%1 is used by \"%2\". Take it over?").arg(key.toString(), commandName(other))))
                return false;
            auto left = other->shortcuts();
            left.removeAll(key);
            other->setShortcuts(left);
            store(other);
        }
    a->setShortcuts(keys);
    store(a);
    return true;
}

// Aegisub's and Kainote's hotkey list: every command, also those only reachable by a key (tabs);
// a hotkey that differs from the default is bold.
void MainWindow::editHotkeys() {
    // the macros of the autoload scripts are listed too; loading them later would also build
    // the automation menu anew under the list
    ensureAutoload();
    QDialog d(this);
    d.setWindowTitle(tr("Hotkeys"));
    d.resize(680, 600);
    auto *v = new QVBoxLayout(&d);
    auto *filter = new QLineEdit(&d);
    filter->setPlaceholderText(tr("Search"));
    filter->setClearButtonEnabled(true);
    v->addWidget(filter);
    auto *list = new QTreeWidget(&d);
    list->setHeaderLabels({tr("Command"), tr("Menu"), tr("Hotkey")});
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    v->addWidget(list, 1);
    const int side = style()->pixelMetric(QStyle::PM_SmallIconSize);
    QPixmap blank(side, side); // keeps names without an icon in line
    blank.fill(Qt::transparent);
    for (QAction *a : std::as_const(m_actions)) {
        if (a->menu()) continue; // a submenu (recent files) has nothing to run
        QString menu;
        for (QObject *o : a->associatedObjects())
            if (auto *m = qobject_cast<QMenu *>(o)) {
                menu = fs::plainText(m->title());
                break;
            }
        auto *it = new QTreeWidgetItem(list, {commandName(a), menu});
        it->setIcon(0, a->icon().isNull() ? QIcon(blank) : a->icon());
        // watched: a script read again while the list is open takes its macros' actions away
        it->setData(0, Qt::UserRole, QVariant::fromValue(QPointer<QAction>(a)));
    }
    list->sortItems(1, Qt::AscendingOrder);
    const auto refresh = [list] {
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto *it = list->topLevelItem(i);
            const auto a = it->data(0, Qt::UserRole).value<QPointer<QAction>>();
            it->setDisabled(!a);
            if (!a) continue;
            it->setText(2, QKeySequence::listToString(a->shortcuts(), QKeySequence::NativeText));
            QFont f = it->font(0);
            f.setBold(a->shortcuts() != defaultKeys(a));
            it->setFont(2, f);
        }
    };
    refresh();
    // the keys stay in sight next to a long macro name
    list->header()->setStretchLastSection(false);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c : {1, 2}) list->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    connect(filter, &QLineEdit::textChanged, &d, [list](const QString &s) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto *it = list->topLevelItem(i);
            it->setHidden(!s.isEmpty() && !it->text(0).contains(s, Qt::CaseInsensitive) &&
                          !it->text(2).contains(s, Qt::CaseInsensitive));
        }
    });
    const auto current = [list]() -> QAction * {
        auto *it = list->currentItem();
        return it ? it->data(0, Qt::UserRole).value<QPointer<QAction>>().data() : nullptr;
    };
    const auto change = [=, this] {
        if (auto *a = current()) assignHotkey(a);
        refresh();
    };
    connect(list, &QTreeWidget::itemActivated, &d, change);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    connect(bb->addButton(tr("Change..."), QDialogButtonBox::ActionRole), &QPushButton::clicked, &d, change);
    connect(bb->addButton(tr("Default"), QDialogButtonBox::ActionRole), &QPushButton::clicked, &d, [=, this] {
        if (auto *a = current()) setHotkey(a, defaultKeys(a));
        refresh();
    });
    connect(bb->addButton(tr("All defaults"), QDialogButtonBox::ResetRole), &QPushButton::clicked, &d, [=, this] {
        if (!confirm(tr("Restore all hotkeys to their defaults?"))) return;
        QSettings().remove("hotkeys");
        for (auto *a : std::as_const(m_actions)) {
            a->setShortcut(a->property("defaultKey").value<QKeySequence>());
            showHotkey(a);
        }
        refresh();
    });
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    d.exec();
}

// Shift+click on a menu item assigns its hotkey; also routes keys to the
// panels that use plain keys (video, audio, grid, edit box) before the
// global shortcuts take them, like Kainote's per-context hotkeys.
bool MainWindow::eventFilter(QObject *o, QEvent *e) {
    // files dropped anywhere in the window are opened; text fields would paste the path otherwise
    if (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove || e->type() == QEvent::Drop) {
        auto *de = static_cast<QDropEvent *>(e);
        auto *w = qobject_cast<QWidget *>(o);
        if (w && w != this && w->window() == this && de->mimeData()->hasUrls() &&
            de->mimeData()->urls().value(0).isLocalFile()) {
            if (e->type() == QEvent::Drop) dropEvent(de);
            else de->acceptProposedAction();
            return true;
        }
    }
    // a toolbar button dragged onto another takes its place (Kainote); menu buttons open on press
    if ((e->type() == QEvent::MouseMove || e->type() == QEvent::MouseButtonRelease) && m_toolbar &&
        o->parent() == m_toolbar)
        if (auto *src = qobject_cast<QToolButton *>(o); src && src->defaultAction()) {
            auto *me = static_cast<QMouseEvent *>(e);
            auto *dst = qobject_cast<QToolButton *>(QApplication::widgetAt(me->globalPosition().toPoint()));
            const bool onOther = dst && dst != src && dst->parent() == m_toolbar && dst->defaultAction();
            // the button whose place the drop takes is shown pressed
            static QPointer<QToolButton> target;
            auto mark = [](QToolButton *b) {
                if (target == b) return;
                if (target) target->setDown(false);
                if ((target = b)) b->setDown(true);
            };
            if (e->type() == QEvent::MouseMove && (me->buttons() & Qt::LeftButton)) {
                src->setCursor(onOther ? Qt::DragMoveCursor : Qt::ArrowCursor);
                mark(onOther ? dst : nullptr);
            } else if (e->type() == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton) {
                src->unsetCursor();
                mark(nullptr);
                QStringList l = QSettings().value("toolbar/ids", kToolbarDefault).toStringList();
                const int from = l.indexOf(m_actions.key(src->defaultAction()));
                const int to = onOther ? l.indexOf(m_actions.key(dst->defaultAction())) : -1;
                if (from >= 0 && to >= 0) {
                    l.move(from, to);
                    QSettings().setValue("toolbar/ids", l);
                    QTimer::singleShot(0, this, [this] { buildToolbar(); });
                }
            }
        }
    // Kainote: Shift+click on a button assigns the key of its command; its tooltip names the key
    if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick || e->type() == QEvent::ToolTip)
        if (auto *b = qobject_cast<QAbstractButton *>(o); b && b->window() == this) {
            QString id = b->property("command").toString();
            if (auto *tb = qobject_cast<QToolButton *>(b); id.isEmpty() && tb && tb->defaultAction())
                id = tb->defaultAction()->objectName();
            if (QAction *a = id.isEmpty() ? nullptr : action(id)) {
                if (e->type() == QEvent::ToolTip) {
                    const QString key = a->shortcut().toString(QKeySequence::NativeText);
                    if (key.isEmpty() || b->toolTip().isEmpty()) return false;
                    QToolTip::showText(static_cast<QHelpEvent *>(e)->globalPos(), b->toolTip() + " (" + key + ")", b);
                    return true;
                }
                auto *me = static_cast<QMouseEvent *>(e);
                if (me->button() == Qt::LeftButton && (me->modifiers() & Qt::ShiftModifier)) {
                    QTimer::singleShot(0, this, [this, id] {
                        if (QAction *later = action(id)) assignHotkey(later);
                    });
                    return true;
                }
            }
        }
    // a click in the split view's right half makes that tab active (the sides swap)
    if (e->type() == QEvent::MouseButtonPress && m_second)
        if (auto *w = qobject_cast<QWidget *>(o); w && m_second->isAncestorOf(w))
            QTimer::singleShot(0, this, [this] {
                if (m_second) m_tabs->setCurrentIndex(indexOfPanel(m_second));
            });
    if (e->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (auto *menu = qobject_cast<QMenu *>(o); menu && (me->modifiers() & Qt::ShiftModifier)) {
            if (auto *a = menu->actionAt(me->position().toPoint()); a && !a->menu() && !a->objectName().isEmpty()) {
                menu->close();
                QTimer::singleShot(0, this, [this, a] { assignHotkey(a); });
                return true;
            }
        }
        if (o == m_tabs->tabBar() && me->button() == Qt::MiddleButton) {
            const int i = m_tabs->tabBar()->tabAt(me->position().toPoint());
            if (i >= 0) QTimer::singleShot(0, this, [this, i] { closeTab(i); });
            return true;
        }
    }
    // Medusa mode: the audio's keys that type nothing (the keypad's, with Ctrl or Alt, F keys) time
    // the line from anywhere outside the video and audio, which keep their own keys
    if ((e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress) && m_medusa && current() &&
        !dynamic_cast<VideoView *>(o) && !dynamic_cast<AudioView *>(o)) {
        auto *ke = static_cast<QKeyEvent *>(e);
        auto *w = qobject_cast<QWidget *>(o);
        const bool typesNothing = (ke->modifiers() & (Qt::KeypadModifier | Qt::ControlModifier | Qt::AltModifier)) ||
                                  (ke->key() >= Qt::Key_F1 && ke->key() <= Qt::Key_F35);
        if (w && w->window() == this && typesNothing && current()->audio()->hasAudio())
            if (const char *id = panelCommand(ke, AudioBox::keys())) {
                if (e->type() == QEvent::KeyPress) current()->audio()->runCommand(id);
                e->accept();
                return true;
            }
    }
    if (e->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(e);
        const auto mods = ke->modifiers() & ~Qt::KeypadModifier;
        const int k = ke->key();
        bool wanted = false;
        if (auto *vv = dynamic_cast<VideoView *>(o); vv || dynamic_cast<AudioView *>(o)) {
            // the panel's own commands take their keys even with Ctrl or Alt
            wanted = (!(mods & Qt::ControlModifier) && !(mods & Qt::AltModifier) && k < Qt::Key_F1) ||
                     panelCommand(ke, vv ? VideoBox::keys() : AudioBox::keys());
            // Ctrl+A picks every point of a vector clip or drawing
            if (const int t = current()->video()->tools()->tool(); vv && (t == VisualTools::VClip || t == VisualTools::Draw))
                wanted = wanted || ke->matches(QKeySequence::SelectAll);
        } else if (dynamic_cast<Grid *>(o)) {
            // Alt+arrows are left to the window's commands (Aegisub's grid/move/up|down)
            wanted = (!(mods & (Qt::ControlModifier | Qt::AltModifier)) &&
                      (k == Qt::Key_Up || k == Qt::Key_Down || k == Qt::Key_PageUp || k == Qt::Key_PageDown ||
                       k == Qt::Key_Home || k == Qt::Key_End || k == Qt::Key_Return || k == Qt::Key_Enter)) ||
                     ke->matches(QKeySequence::SelectAll) || ke->matches(QKeySequence::Copy) ||
                     ke->matches(QKeySequence::Cut) || ke->matches(QKeySequence::Paste);
        } else if (dynamic_cast<SubsTextEdit *>(o)) {
            wanted = panelCommand(ke, EditBox::keys());
        }
        if (wanted) {
            e->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(o, e);
}

const char *MainWindow::panelCommand(const QKeyEvent *e, std::span<const PanelKey> keys) const {
    // composed text (a dead key, Alt+numpad) has no key: its empty sequence would match every unbound command
    if (e->key() == 0 || e->key() == Qt::Key_unknown) return nullptr;
    const auto key = Qt::Key(e->key() == Qt::Key_Enter ? Qt::Key_Return : e->key());
    for (const auto mods : {e->modifiers(), e->modifiers() & ~Qt::KeypadModifier}) {
        const QKeySequence pressed(QKeyCombination(mods, key));
        for (const auto &k : keys)
            if (auto *a = action(k.id); a && a->shortcuts().contains(pressed)) return k.id;
    }
    return nullptr;
}
