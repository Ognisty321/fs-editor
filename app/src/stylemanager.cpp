#include "stylemanager.h"

#include "dialogutil.h"
#include "icons.h"
#include "mainwindow.h"
#include "styleeditor.h"
#include "tabpanel.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHash>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <numeric>
#include <utility>
#include <vector>

namespace {
template <class List> QStringList namesOf(const List &styles) {
    QStringList l;
    for (const auto &s : styles) l << fs::q(s.name);
    return l;
}
// Aegisub's unique_name: "X - copy", then "X - copy (2)"...
QString uniqueName(const QString &first, const QStringList &taken) {
    QString n = first;
    for (int i = 2; taken.contains(n); ++i) n = QString("%1 (%2)").arg(first).arg(i);
    return n;
}
// Aegisub's do_move; false when the block is already there
template <class V> bool moveBlock(V &v, int type, int &first, int &last) {
    const auto b = v.begin();
    const int n = int(v.size()), len = last - first + 1;
    if ((type < 2 && first == 0) || (type >= 2 && last == n - 1)) return false;
    if (type == 0) std::rotate(b, b + first, b + last + 1);
    if (type == 1) std::rotate(b + first - 1, b + first, b + last + 1);
    if (type == 2) std::rotate(b + first, b + last + 1, b + last + 2);
    if (type == 3) std::rotate(b + first, b + last + 1, v.end());
    first = type == 0 ? 0 : type == 1 ? first - 1 : type == 2 ? first + 1 : n - len;
    last = first + len - 1;
    return true;
}
} // namespace

StyleManager::StyleManager(MainWindow *main) : QDialog(main), m_main(main) {
    setWindowTitle(tr("Style manager"));
    auto *g = new QGridLayout(this);
    auto *catRow = new QHBoxLayout;
    m_catalog = new QComboBox(this);
    const QDir dir = QFileInfo(catalogFile("x")).dir();
    dir.mkpath(".");
    QSettings st;
    // catalogs kept in the settings before become their files, once
    for (const QString &n : st.value("styles/catalogs").toStringList()) {
        QFile f(catalogFile(QString(n).replace(QRegularExpression(R"([\\/:*?"<>|])"), "_")));
        if (!f.exists() && f.open(QIODevice::WriteOnly))
            f.write("\xEF\xBB\xBF" + st.value("styles/catalog/" + n).toStringList().join('\n').toUtf8() + '\n');
        if (f.exists()) st.remove("styles/catalog/" + n);
    }
    st.remove("styles/catalogs");
    QStringList cats = styleCatalogs();
    if (cats.isEmpty()) cats << "Default";
    m_catalog->addItems(cats);
    auto *newCat = new QPushButton(tr("New"), this);
    auto *delCat = m_delCat = new QPushButton(tr("Delete"), this);
    catRow->addWidget(new QLabel(tr("Catalog:"), this));
    catRow->addWidget(m_catalog, 1);
    catRow->addWidget(newCat);
    catRow->addWidget(delCat);
    g->addLayout(catRow, 0, 0, 1, 2);
    g->addWidget(new QLabel(tr("Styles stored"), this), 1, 0);
    m_storage = new QListWidget(this);
    m_storage->setSelectionMode(QAbstractItemView::ExtendedSelection);
    g->addWidget(m_storage, 2, 0);
    auto *sb = new QVBoxLayout;
    auto *sNew = new QPushButton(tr("New"), this), *sCopy = new QPushButton(tr("Copy"), this),
         *sEdit = new QPushButton(tr("Edit"), this), *sDel = new QPushButton(tr("Delete"), this),
         *sSort = new QPushButton(tr("Sort"), this), *sImport = new QPushButton(tr("Import..."), this);
    sImport->setToolTip(tr("Import styles from a subtitle file into the catalog"));
    for (auto *b : {sNew, sCopy, sEdit, sDel, sSort, sImport}) sb->addWidget(b);
    sb->addStretch();
    g->addLayout(sb, 2, 1);
    auto *mid = new QHBoxLayout;
    auto *toStore = new QPushButton(tr("Add to storage ▲"), this);
    auto *toFile = new QPushButton(tr("Add to ASS ▼"), this);
    auto *toAll = new QPushButton(tr("Add to all open ASS files"), this);
    for (auto *b : {toStore, toFile, toAll}) mid->addWidget(b);
    g->addLayout(mid, 3, 0, 1, 2);
    g->addWidget(new QLabel(tr("Styles in ASS file"), this), 4, 0);
    m_file = new QListWidget(this);
    m_file->setSelectionMode(QAbstractItemView::ExtendedSelection);
    g->addWidget(m_file, 5, 0);
    auto *fb = new QVBoxLayout;
    auto *fNew = new QPushButton(tr("New"), this), *fCopy = new QPushButton(tr("Copy"), this),
         *fEdit = new QPushButton(tr("Edit"), this), *fDel = new QPushButton(tr("Delete"), this),
         *fClear = new QPushButton(tr("Clear"), this), *fClean = new QPushButton(tr("Clean"), this),
         *fSort = new QPushButton(tr("Sort"), this), *fImport = new QPushButton(tr("Import..."), this);
    fClean->setToolTip(tr("Delete unused styles"));
    fImport->setToolTip(tr("Import styles from another subtitle file"));
    for (auto *b : {fNew, fCopy, fEdit, fDel, fClear, fClean, fSort, fImport}) fb->addWidget(b);
    fb->addStretch();
    g->addLayout(fb, 5, 1);
    // Edit with several selected: the others take what changes in the current one (Kainote)
    m_forOne[0] = {sCopy};
    m_forAny[0] = {sEdit, sDel, toFile, toAll};
    m_forOne[1] = {fCopy};
    m_forAny[1] = {fEdit, fDel, toStore};
    const auto moveButtons = [this](QBoxLayout *column, bool storage) {
        static const char *const names[] = {"arrow-bar-to-up", "arrow-up", "arrow-down", "arrow-bar-to-down"};
        static const char *const tips[] = {QT_TR_NOOP("Move to top"), QT_TR_NOOP("Move up"), QT_TR_NOOP("Move down"),
                                           QT_TR_NOOP("Move to bottom")};
        auto *row = new QHBoxLayout;
        for (int k = 0; k < 4; ++k) {
            auto *b = new QToolButton(this);
            b->setIcon(icon(names[k]));
            b->setToolTip(tr(tips[k]));
            connect(b, &QToolButton::clicked, this, [=, this] { moveStyles(storage, k); });
            row->addWidget(b);
            m_moves[storage ? 0 : 1][k] = b;
        }
        column->insertLayout(column->count() - 1, row); // above the stretch
    };
    moveButtons(sb, true);
    moveButtons(fb, false);
    auto *close = new QPushButton(tr("Close"), this);
    g->addWidget(close, 6, 1);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    // Kainote keeps the style editor in this window, beside the lists; Aegisub opens a window
    auto *dock = new QCheckBox(tr("Edit styles in this window"), this);
    dock->setToolTip(tr("The style editor stands beside the lists and stays open while you pick other styles"));
    dock->setChecked(QSettings().value("styles/dockEditor", false).toBool());
    g->addWidget(dock, 6, 0);
    connect(dock, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("styles/dockEditor", on);
        if (!on) closeEditor();
    });

    auto session = [this]() -> fsb::Session & { return m_main->current()->s(); };
    auto edited = [this] {
        m_main->current()->changed();
        reload();
    };
    auto defaultStyle = [] {
        fsb::StyleData d;
        d.name = "Default";
        d.font = "Arial";
        d.size = 48;
        d.primary = "&H00FFFFFF";
        d.secondary = "&H000000FF";
        d.outline_color = "&H00000000";
        d.back = "&H00000000";
        d.scale_x = d.scale_y = 100;
        d.border_style = 1;
        d.outline = 2;
        d.shadow = 2;
        d.alignment = 2;
        d.margin_l = d.margin_r = d.margin_v = 10;
        d.encoding = 1;
        return d;
    };
    connect(fNew, &QPushButton::clicked, this, [=, this] {
        auto d = defaultStyle();
        d.name = fs::rs(uniqueName("Default", namesOf(session().styles())));
        openEditor(d, fileApply({}, {}));
    });
    // Copy opens the editor on the copy, which OK adds (Aegisub)
    connect(fCopy, &QPushButton::clicked, this, [=, this] {
        const int i = m_file->currentRow();
        if (i < 0) return;
        const auto styles = session().styles();
        auto s = styles[size_t(i)];
        s.name = fs::rs(uniqueName(fs::q(s.name) + tr(" - copy"), namesOf(styles)));
        openEditor(s, fileApply({}, {}));
    });
    auto editFile = [=, this] {
        const int i = m_file->currentRow();
        if (i < 0) return;
        const auto style = session().styles()[size_t(i)];
        openEditor(style, fileApply(fs::q(style.name), otherSelected(m_file)));
    };
    connect(fEdit, &QPushButton::clicked, this, editFile);
    connect(m_file, &QListWidget::itemActivated, this, editFile); // a double click or Enter
    connect(fDel, &QPushButton::clicked, this, [=, this] {
        std::vector<size_t> rows;
        for (const auto &r : m_file->selectionModel()->selectedRows()) rows.push_back(size_t(r.row()));
        if (rows.empty() || !m_main->confirm(tr("Delete the selected styles?"))) return;
        session().delete_styles({rows.data(), rows.size()});
        edited();
    });
    connect(fClean, &QPushButton::clicked, this, [=, this] {
        QStringList gone;
        for (const auto &n : session().clean_styles()) gone << fs::q(n);
        edited();
        QMessageBox::information(this, tr("Delete unused styles"),
                                 gone.isEmpty() ? tr("Every style is in use.")
                                                : tr("Deleted styles:\n%1").arg(gone.join("\n")));
    });
    connect(fClear, &QPushButton::clicked, this, [=, this] {
        if (!m_main->confirm(tr("Delete all styles from the file?"))) return;
        std::vector<size_t> all(session().styles().size());
        std::iota(all.begin(), all.end(), size_t(0));
        session().delete_styles({all.data(), all.size()});
        edited();
    });
    const auto selectedStored = [this] {
        std::vector<fsb::StyleData> v;
        for (int r = 0; r < m_storage->count(); ++r)
            if (m_storage->item(r)->isSelected()) v.push_back(m_stored[size_t(r)]);
        return v;
    };
    connect(toFile, &QPushButton::clicked, this, [=, this] {
        const auto chosen = askOverwrite(selectedStored(), namesOf(session().styles()));
        if (chosen.empty()) return;
        session().add_styles({chosen.data(), chosen.size()});
        edited();
    });
    connect(toAll, &QPushButton::clicked, this, [=, this] {
        QStringList taken;
        for (auto *tab : m_main->tabs()) taken << namesOf(tab->s().styles());
        const auto chosen = askOverwrite(selectedStored(), taken);
        if (chosen.empty()) return;
        for (auto *tab : m_main->tabs()) {
            tab->s().add_styles({chosen.data(), chosen.size()});
            tab->changed();
        }
        reload();
    });
    connect(fSort, &QPushButton::clicked, this, [=, this] {
        const auto styles = session().styles();
        std::vector<size_t> order(styles.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return fs::q(styles[a].name) < fs::q(styles[b].name); });
        if (std::is_sorted(order.begin(), order.end())) return;
        session().reorder_styles({order.data(), order.size()});
        edited();
    });
    // Aegisub's "Import from script": pick styles of another file, asking before overwriting;
    // Kainote's "Load" does the same for the catalog
    const auto importStyles = [=, this](bool storage) {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import styles"),
                                                          QSettings().value("main/lastDir").toString(),
                                                          tr("ASS/SSA subtitles (*.ass *.ssa);;All files (*)"));
        if (path.isEmpty()) return;
        auto other = fsb::new_session();
        try {
            other->open(fs::Utf8(path));
        } catch (const rust::Error &e) {
            return (void)QMessageBox::warning(this, tr("Import styles"), QString::fromUtf8(e.what()));
        }
        const auto styles = other->styles();
        if (styles.empty())
            return (void)QMessageBox::information(this, tr("Import styles"),
                                                  tr("The selected file has no available styles."));
        QDialog pick(this);
        pick.setWindowTitle(tr("Import styles"));
        auto *l = new QVBoxLayout(&pick);
        l->addWidget(new QLabel(tr("Choose styles to import:"), &pick));
        auto *list = new QListWidget(&pick);
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        for (const auto &s : styles) list->addItem(fs::q(s.name));
        list->selectAll();
        l->addWidget(list);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &pick);
        connect(bb, &QDialogButtonBox::accepted, &pick, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &pick, &QDialog::reject);
        l->addWidget(bb);
        if (pick.exec() != QDialog::Accepted) return;
        std::vector<fsb::StyleData> picked;
        for (int r = 0; r < list->count(); ++r)
            if (list->item(r)->isSelected()) picked.push_back(styles[size_t(r)]);
        if (storage) return addToStorage(picked);
        const auto chosen = askOverwrite(picked, namesOf(session().styles()));
        if (chosen.empty()) return;
        session().add_styles({chosen.data(), chosen.size()});
        edited();
    };
    connect(fImport, &QPushButton::clicked, this, [importStyles] { importStyles(false); });
    connect(sImport, &QPushButton::clicked, this, [importStyles] { importStyles(true); });
    connect(toStore, &QPushButton::clicked, this, [=, this] {
        const auto styles = session().styles();
        std::vector<fsb::StyleData> picked;
        for (auto *it : m_file->selectedItems()) picked.push_back(styles[size_t(m_file->row(it))]);
        addToStorage(picked);
    });
    connect(sNew, &QPushButton::clicked, this, [=, this] {
        auto d = defaultStyle();
        d.name = fs::rs(uniqueName("Default", namesOf(m_stored)));
        openEditor(d, storageApply({}, {}));
    });
    auto editStored = [this] {
        const int i = m_storage->currentRow();
        if (i < 0) return;
        const auto style = m_stored[size_t(i)];
        openEditor(style, storageApply(fs::q(style.name), otherSelected(m_storage)));
    };
    connect(sEdit, &QPushButton::clicked, this, editStored);
    connect(m_storage, &QListWidget::itemActivated, this, editStored);
    connect(sCopy, &QPushButton::clicked, this, [this] {
        const int i = m_storage->currentRow();
        if (i < 0) return;
        auto s = m_stored[size_t(i)];
        s.name = fs::rs(uniqueName(fs::q(s.name) + tr(" - copy"), namesOf(m_stored)));
        openEditor(s, storageApply({}, {}));
    });
    connect(sDel, &QPushButton::clicked, this, [this] {
        auto rows = m_storage->selectionModel()->selectedRows();
        if (rows.empty() || !m_main->confirm(tr("Delete the selected styles?"))) return;
        std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.row() > b.row(); });
        for (const auto &r : rows) m_stored.erase(m_stored.begin() + r.row());
        saveCatalog();
        reload();
    });
    connect(sSort, &QPushButton::clicked, this, [this] {
        std::sort(m_stored.begin(), m_stored.end(), [](auto &a, auto &b) { return fs::q(a.name) < fs::q(b.name); });
        saveCatalog();
        reload();
    });
    connect(newCat, &QPushButton::clicked, this, [this] {
        bool ok;
        QString n = QInputDialog::getText(this, tr("Catalog"), tr("Name"), QLineEdit::Normal, {}, &ok).trimmed();
        n.replace(QRegularExpression(R"([\\/:*?"<>|])"), "_"); // it names a file (Aegisub)
        if (!ok || n.isEmpty()) return;
        // file names ignore case on Windows: the one there is
        if (const int k = m_catalog->findText(n, Qt::MatchFixedString); k >= 0) return m_catalog->setCurrentIndex(k);
        m_catalog->addItem(n);
        m_catalog->setCurrentText(n);
        saveCatalog(); // listed next time even while empty
    });
    connect(delCat, &QPushButton::clicked, this, [this] {
        if (m_catalog->count() <= 1 || !m_main->confirm(tr("Delete the catalog \"%1\"?").arg(m_catalog->currentText())))
            return;
        QFile::remove(catalogFile(m_catalog->currentText()));
        m_catalog->removeItem(m_catalog->currentIndex());
    });
    connect(m_catalog, &QComboBox::currentTextChanged, this, [this] {
        closeEditor(); // it may hold a style of the catalog left
        loadCatalog();
        reload();
        // with the file's project settings, as Aegisub keeps it: not an edit to undo
        m_main->current()->s().set_project_value("Last Style Storage", fs::Utf8(m_catalog->currentText()));
    });
    // Delete, Ctrl+C and Ctrl+V on the lists (Kainote); Enter edits, so no button is the default
    for (const bool storage : {true, false}) {
        QListWidget *list = storage ? m_storage : m_file;
        const auto key = [this, list](QKeySequence k, auto fn) {
            auto *a = new QAction(list);
            a->setShortcut(k);
            a->setShortcutContext(Qt::WidgetShortcut);
            connect(a, &QAction::triggered, this, fn);
            list->addAction(a);
        };
        key(QKeySequence::Delete, [b = storage ? sDel : fDel] { b->click(); });
        key(QKeySequence::Copy, [this, storage] { copyStyles(storage); });
        key(QKeySequence::Paste, [this, storage] { pasteStyles(storage); });
        connect(list, &QListWidget::itemSelectionChanged, this, &StyleManager::updateButtons);
    }
    for (auto *b : findChildren<QPushButton *>()) b->setAutoDefault(false);
    const QString last = fs::q(m_main->current()->s().project_value("Last Style Storage"));
    if (!last.isEmpty() && cats.contains(last)) m_catalog->setCurrentText(last);
    loadCatalog();
    tabChanged();
    // the active line's style picked (Aegisub)
    if (auto *tab = m_main->current(); tab->active() >= 0)
        if (const auto items = m_file->findItems(fs::q(tab->s().row(size_t(tab->active())).style), Qt::MatchExactly);
            !items.isEmpty())
            m_file->setCurrentItem(items.first());
    resize(560, 640);
}

// The editor on `style`: a window of its own that is waited for (Aegisub), or, with
// styles/dockEditor, a part of this window that the next style edited takes over (Kainote).
void StyleManager::openEditor(const fsb::StyleData &style, std::function<bool(const fsb::StyleData &)> apply) {
    const bool replaces = m_editor; // a docked one: the window keeps the size it has with it
    if (!replaces) m_undocked = size();
    removeEditor();
    if (!QSettings().value("styles/dockEditor", false).toBool()) {
        StyleEditor e(style, this);
        e.onApply = std::move(apply);
        e.exec();
        return;
    }
    auto *e = new StyleEditor(style, this);
    e->setWindowFlags(Qt::Widget);
    e->onApply = std::move(apply);
    m_editor = e;
    static_cast<QGridLayout *>(layout())->addWidget(e, 0, 2, 7, 1);
    connect(e, &QDialog::finished, this, [this, e] {
        if (m_editor == e) closeEditor(); // OK, Cancel or Esc in it
    });
    e->show();
    if (!replaces)
        resize(m_undocked.width() + e->sizeHint().width(), std::max(m_undocked.height(), e->sizeHint().height()));
    e->setFocus();
}

// The docked editor taken out of the window, which keeps its size.
void StyleManager::removeEditor() {
    if (!m_editor) return;
    StyleEditor *const e = m_editor;
    m_editor = nullptr;
    layout()->removeWidget(e);
    e->hide();
    e->deleteLater();
}

void StyleManager::closeEditor() {
    if (!m_editor) return;
    removeEditor();
    // once the layout has let go of the editor's width
    QTimer::singleShot(0, this, [this] {
        if (!m_editor) resize(m_undocked);
    });
}

namespace {
template <class List> int rowOfStyle(const List &styles, const QString &name) {
    for (size_t k = 0; k < styles.size(); ++k)
        if (fs::q(styles[k].name) == name) return int(k);
    return -1;
}
} // namespace

// What the editor's Apply and OK do with a style of the file: `name` is the style edited (none
// for a new one) and `others` the styles that take the same changes (Kainote's edit of several).
// Both are found again by name each time, since a docked editor stays open while the list
// changes; a style deleted meanwhile is added back.
std::function<bool(const fsb::StyleData &)> StyleManager::fileApply(QString name, QStringList others) {
    return [this, name, others](const fsb::StyleData &s) mutable {
        auto &session = m_main->current()->s();
        const auto styles = session.styles();
        int idx = name.isEmpty() ? -1 : rowOfStyle(styles, name);
        const bool had = idx >= 0;
        const fsb::StyleData before = had ? styles[size_t(idx)] : s;
        if (!applyToFile(idx, s)) return false;
        name = fs::q(s.name);
        if (!had || others.isEmpty()) return true;
        std::vector<size_t> rows;
        const auto now = session.styles();
        for (const QString &o : others)
            if (const int r = rowOfStyle(now, o); r >= 0 && r != idx) rows.push_back(size_t(r));
        if (changeSelected(rows, before, s) && session.change_styles(before, s, fs::slice(rows))) {
            m_main->current()->changed();
            reload();
        }
        return true;
    };
}

// The same for a style of the catalog shown.
std::function<bool(const fsb::StyleData &)> StyleManager::storageApply(QString name, QStringList others) {
    return [this, name, others](const fsb::StyleData &s) mutable {
        int idx = name.isEmpty() ? -1 : rowOfStyle(m_stored, name);
        const bool had = idx >= 0;
        const fsb::StyleData before = had ? m_stored[size_t(idx)] : s;
        if (!applyToStorage(idx, s)) return false;
        name = fs::q(s.name);
        if (!had || others.isEmpty()) return true;
        std::vector<size_t> rows;
        for (const QString &o : others)
            if (const int r = rowOfStyle(m_stored, o); r >= 0 && r != idx) rows.push_back(size_t(r));
        if (changeSelected(rows, before, s)) {
            for (size_t k : rows) m_stored[k] = fsb::style_with_changes(before, s, m_stored[k]);
            saveCatalog();
            reload();
        }
        return true;
    };
}

void StyleManager::tabChanged() {
    closeEditor(); // it may hold a style of the file left
    disconnect(m_docConn);
    if (auto *tab = m_main->current())
        m_docConn = connect(tab, &TabPanel::docChanged, this, [this] {
            if (isVisible()) reload(); // an undo, a style edited in the line's box
        });
    reload();
}

void StyleManager::updateButtons() {
    m_delCat->setEnabled(m_catalog->count() > 1);
    for (int k = 0; k < 2; ++k) {
        QListWidget *list = k ? m_file : m_storage;
        int first = -1, last = -1, n = 0;
        for (int r = 0; r < list->count(); ++r)
            if (list->item(r)->isSelected()) {
                if (first < 0) first = r;
                last = r;
                ++n;
            }
        for (auto *b : m_forOne[k]) b->setEnabled(n == 1);
        for (auto *b : m_forAny[k]) b->setEnabled(n > 0);
        // a block moves only when it is one
        const bool block = n > 0 && last - first + 1 == n;
        for (int m = 0; m < 4; ++m) m_moves[k][m]->setEnabled(block && (m < 2 ? first > 0 : last < list->count() - 1));
    }
}

void StyleManager::addToStorage(const std::vector<fsb::StyleData> &styles) {
    for (const auto &st : askOverwrite(styles, namesOf(m_stored))) {
        auto same = std::find_if(m_stored.begin(), m_stored.end(), [&](auto &x) { return fs::q(x.name) == fs::q(st.name); });
        if (same != m_stored.end()) *same = st;
        else m_stored.push_back(st);
    }
    saveCatalog();
    reload();
}

void StyleManager::copyStyles(bool storage) {
    std::vector<fsb::StyleData> v;
    const auto styles = m_main->current()->s().styles();
    QListWidget *list = storage ? m_storage : m_file;
    for (int r = 0; r < list->count(); ++r)
        if (list->item(r)->isSelected()) v.push_back(storage ? m_stored[size_t(r)] : styles[size_t(r)]);
    if (!v.empty()) QApplication::clipboard()->setText(fs::q(fsb::style_lines({v.data(), v.size()})));
}

void StyleManager::pasteStyles(bool storage) {
    const auto parsed = fsb::parse_style_lines(fs::Utf8(QApplication::clipboard()->text()));
    const std::vector<fsb::StyleData> v(parsed.begin(), parsed.end());
    if (v.empty()) return;
    if (storage) return addToStorage(v);
    auto &s = m_main->current()->s();
    const auto chosen = askOverwrite(v, namesOf(s.styles()));
    if (chosen.empty()) return;
    s.add_styles({chosen.data(), chosen.size()});
    m_main->current()->changed();
    reload();
}

void StyleManager::loadCatalog() { m_stored = catalogStyles(m_catalog->currentText()); }

void StyleManager::saveCatalog() {
    QSaveFile f(catalogFile(m_catalog->currentText()));
    if (!f.open(QIODevice::WriteOnly)) return m_main->error(f.errorString());
    f.write("\xEF\xBB\xBF"); // UTF-8 with a BOM, as Aegisub writes them
    f.write(fs::q(fsb::style_lines({m_stored.data(), m_stored.size()})).toUtf8());
    if (!f.commit()) m_main->error(f.errorString());
}

void StyleManager::moveStyles(bool storage, int type) {
    QListWidget *list = storage ? m_storage : m_file;
    int first = list->count(), last = -1;
    for (int r = 0; r < list->count(); ++r)
        if (list->item(r)->isSelected()) first = std::min(first, r), last = r;
    if (last < 0) return;
    for (int r = first; r <= last; ++r)
        if (!list->item(r)->isSelected()) return; // Aegisub moves only a block
    if (storage) {
        if (!moveBlock(m_stored, type, first, last)) return;
        saveCatalog();
    } else {
        std::vector<size_t> order(size_t(list->count()));
        std::iota(order.begin(), order.end(), size_t(0));
        if (!moveBlock(order, type, first, last)) return;
        m_main->current()->s().reorder_styles({order.data(), order.size()});
        m_main->current()->changed();
    }
    reload();
    for (int r = first; r <= last; ++r) list->item(r)->setSelected(true);
    list->setCurrentRow(first, QItemSelectionModel::NoUpdate);
}

std::vector<fsb::StyleData> StyleManager::askOverwrite(const std::vector<fsb::StyleData> &chosen,
                                                       const QStringList &taken) {
    std::vector<fsb::StyleData> out;
    bool all = false;
    for (const auto &s : chosen) {
        if (!all && taken.contains(fs::q(s.name))) {
            const auto b = QMessageBox::question(this, tr("Style name collision"),
                                                 tr("A style named \"%1\" already exists. Replace it?").arg(fs::q(s.name)),
                                                 QMessageBox::Yes | QMessageBox::YesToAll | QMessageBox::No |
                                                     QMessageBox::Cancel);
            if (b == QMessageBox::No) continue;
            if (b != QMessageBox::Yes && b != QMessageBox::YesToAll) return {}; // Cancel, or no answer
            all = b == QMessageBox::YesToAll;
        }
        out.push_back(s);
    }
    return out;
}

// The families of a font attached to the file, registered with Qt once a file name and size.
static QStringList attachedFamilies(const fsb::Session &s, const fsb::AttachmentRow &a) {
    static QHash<QString, QStringList> known;
    const QString key = fs::q(a.name) + '/' + QString::number(a.size);
    if (const auto it = known.constFind(key); it != known.cend()) return *it;
    const auto data = s.attachment_data(true, a.name);
    const int id = QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size())));
    return known[key] = id < 0 ? QStringList() : QFontDatabase::applicationFontFamilies(id);
}

void StyleManager::reload() {
    // the system's fonts as Qt read them at the start (it does not watch for new ones), and the
    // file's attached ones, which libass draws with as well
    static const QSet<QString> system = [] {
        QSet<QString> f;
        for (const QString &n : QFontDatabase::families()) f.insert(n.toLower());
        return f;
    }();
    QSet<QString> fonts = system;
    if (auto *tab = m_main->current())
        for (const auto &a : tab->s().attachments())
            if (a.font)
                for (const QString &f : attachedFamilies(tab->s(), a)) fonts.insert(f.toLower());
    const auto refill = [this, &fonts](QListWidget *list, const auto &styles) {
        QSet<QString> selected;
        for (auto *it : list->selectedItems()) selected.insert(it->text());
        const QString current = list->currentItem() ? list->currentItem()->text() : QString();
        const QSignalBlocker block(list);
        list->clear();
        for (const auto &s : styles) {
            auto *it = new QListWidgetItem(fs::q(s.name), list);
            QString font = fs::q(s.font);
            if (font.startsWith('@')) font.remove(0, 1); // vertical text
            if (!fonts.contains(font.toLower())) { // Kainote marks them
                it->setForeground(QColor(230, 70, 70));
                it->setToolTip(tr("The font \"%1\" is not installed").arg(font));
            }
            it->setSelected(selected.contains(it->text()));
            if (it->text() == current) list->setCurrentItem(it, QItemSelectionModel::NoUpdate);
        }
    };
    refill(m_storage, m_stored);
    if (auto *tab = m_main->current()) refill(m_file, tab->s().styles());
    updateButtons();
}

void StyleManager::editStyle(const QString &name) {
    const auto items = m_file->findItems(name, Qt::MatchExactly);
    if (items.isEmpty()) return;
    m_file->setCurrentItem(items.first());
    openEditor(m_main->current()->s().styles()[size_t(m_file->currentRow())], fileApply(name, {}));
}

bool StyleManager::applyToFile(int &index, const fsb::StyleData &s) {
    auto &session = m_main->current()->s();
    const auto styles = session.styles();
    for (size_t k = 0; k < styles.size(); ++k)
        if (int(k) != index && fs::q(styles[k].name) == fs::q(s.name)) {
            QMessageBox::warning(this, tr("Style name conflict"),
                                 tr("There is already a style with this name. Please choose another name."));
            return false;
        }
    // Aegisub asks whether the lines (and \r tags) that used the old name follow a rename
    bool lines = true;
    if (index >= 0 && fs::q(styles[size_t(index)].name) != fs::q(s.name) &&
        session.style_in_use(fs::Utf8(fs::q(styles[size_t(index)].name)))) {
        const auto b = QMessageBox::question(
            this, tr("Update script?"), tr("Do you want to change all instances of this style in the script to this new name?"),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (b == QMessageBox::Cancel) return false;
        lines = b == QMessageBox::Yes;
    }
    session.set_style(index, s, lines);
    if (index < 0) index = int(styles.size());
    m_main->current()->changed();
    reload();
    return true;
}

QStringList StyleManager::otherSelected(QListWidget *list) {
    QStringList names;
    for (auto *it : list->selectedItems())
        if (list->row(it) != list->currentRow()) names << it->text();
    return names;
}

bool StyleManager::changeSelected(const std::vector<size_t> &others, const fsb::StyleData &before,
                                  const fsb::StyleData &after) {
    if (others.empty() || fsb::style_with_changes(before, after, before) == before) return false; // a rename only
    return qEnvironmentVariableIsSet("FSED_SMOKE") || // asks nothing in a smoke test
           QMessageBox::question(this, tr("Edit styles"),
                                 tr("Change all the selected styles (%1 more) in the same way?").arg(others.size())) ==
               QMessageBox::Yes;
}

bool StyleManager::applyToStorage(int &index, const fsb::StyleData &s) {
    for (size_t k = 0; k < m_stored.size(); ++k)
        if (int(k) != index && fs::q(m_stored[k].name) == fs::q(s.name)) {
            QMessageBox::warning(this, tr("Style name conflict"),
                                 tr("There is already a style with this name. Please choose another name."));
            return false;
        }
    if (index < 0) {
        index = int(m_stored.size());
        m_stored.push_back(s);
    } else {
        m_stored[size_t(index)] = s;
    }
    saveCatalog();
    reload();
    return true;
}
