// The style manager: the styles of the file beside those of a catalog.
#pragma once

#include "bridge.h"

#include <QDialog>
#include <functional>
#include <vector>

class MainWindow;
class QComboBox;
class QListWidget;
class QPushButton;
class StyleEditor;

class StyleManager : public QDialog {
    Q_OBJECT
public:
    explicit StyleManager(MainWindow *main);
    void editStyle(const QString &name);
    // the window's tab changed: its styles shown, and followed as they change, while this stays open
    void tabChanged();
private:
    // checks the name like Aegisub, then stores the style; index -1 = new, set to where it landed
    bool applyToFile(int &index, const fsb::StyleData &s);
    bool applyToStorage(int &index, const fsb::StyleData &s);
    // the style editor, in its own window or docked in this one (styles/dockEditor)
    void openEditor(const fsb::StyleData &style, std::function<bool(const fsb::StyleData &)> apply);
    void closeEditor(); // the docked one, unapplied; the window goes back to its size without it
    void removeEditor();
    std::function<bool(const fsb::StyleData &)> fileApply(QString name, QStringList others);
    std::function<bool(const fsb::StyleData &)> storageApply(QString name, QStringList others);
    // Kainote's editing of several styles: the names selected besides the current one, and whether
    // their rows take the changes from `before` to `after` (asked)
    static QStringList otherSelected(QListWidget *list);
    bool changeSelected(const std::vector<size_t> &others, const fsb::StyleData &before, const fsb::StyleData &after);
    // Aegisub: the block from the first to the last selected style goes to the top (0), up (1),
    // down (2) or to the bottom (3)
    void moveStyles(bool storage, int type);
    // Kainote: a style replaces one of the same name only when the user says so
    std::vector<fsb::StyleData> askOverwrite(const std::vector<fsb::StyleData> &chosen, const QStringList &taken);
    void reload(); // the selections kept, by name
    void loadCatalog();
    void saveCatalog();
    void updateButtons(); // Aegisub: what the selections allow
    void addToStorage(const std::vector<fsb::StyleData> &styles); // replacing the same names when told to
    void copyStyles(bool storage); // as Style: lines (Kainote)
    void pasteStyles(bool storage);
    MainWindow *m_main;
    QComboBox *m_catalog;
    QListWidget *m_storage, *m_file;
    std::vector<fsb::StyleData> m_stored;
    QMetaObject::Connection m_docConn;
    // [0] the storage's, [1] the file's: buttons for one style, for any, the move arrows
    QList<QWidget *> m_forOne[2], m_forAny[2];
    QWidget *m_moves[2][4];
    QPushButton *m_delCat;
    StyleEditor *m_editor = nullptr; // docked in this window
    QSize m_undocked; // this window's size without it
};
