// Dialogs: find/replace, select lines, style manager and editor, ASS
// properties, history, resample, paste translation, tag buttons, settings.
#pragma once

#include "bridge.h"

#include <QDialog>
#include <array>
#include <functional>
#include <QPointer>
#include <QSet>

class TabPanel;
class MainWindow;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QRadioButton;
class QButtonGroup;
class QListWidget;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;
class QTabWidget;
class QTreeWidget;
class QPlainTextEdit;

class TagButtonDialog : public QDialog {
    Q_OBJECT
public:
    enum { Delete = 2 };
    TagButtonDialog(const QString &name, const QString &tag, int mode, QWidget *parent);
    QString name() const;
    QString tag() const;
    int mode() const;
private:
    QLineEdit *m_name, *m_tag;
    QComboBox *m_mode;
};

class FindDialog : public QDialog {
    Q_OBJECT
public:
    explicit FindDialog(MainWindow *main);
    void showTab(int tab); // 0 find, 1 replace, 2 find in files
    void findNext();
private:
    fsb::FindSpec spec(TabPanel *tab) const;
    bool ready() const; // something to look for: Kainote's start / end of text find empty fields
    void remember(bool replace); // the recent searches and the options, as Aegisub keeps them
    std::array<std::pair<QCheckBox *, const char *>, 7> options() const;
    // Kainote's Find all: the matching lines of the tabs, or of the files in a folder, in a results window
    void findAll(const QList<TabPanel *> &tabs);
    void findInFiles();
    void replaceInFiles(); // Kainote's Replace in subtitles, each file backed up first
    bool filesReady();     // the folder search's settings kept; false without a folder or a valid search
    void clearResults();
    void addResults(TabPanel *tab, const QString &title, const QString &path, const rust::Vec<fsb::FindMatch> &found);
    void showResults(const QString &summary);
    void goToResult(class QTreeWidgetItem *item);
    MainWindow *m_main;
    QTabWidget *m_tabs;
    QComboBox *m_find, *m_replace;
    QCheckBox *m_case, *m_regex, *m_comments, *m_skipTags, *m_skipText, *m_atStart, *m_atEnd, *m_subdirs;
    QRadioButton *m_field[4], *m_lines[3];
    QLineEdit *m_dir, *m_filter;
    class QPushButton *m_findFiles, *m_replFiles;
    QDialog *m_results = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_summary = nullptr;
    QList<QPointer<TabPanel>> m_resultTabs; // per file in the results; null: not open (yet)
    fsb::FindHit m_hit{-1, false, 0, 0}; // the match shown last, where Find and Replace next go on
    QPointer<TabPanel> m_hitTab;
};

class SelectLinesDialog : public QDialog {
    Q_OBJECT
public:
    explicit SelectLinesDialog(MainWindow *main);
private:
    void apply();
    MainWindow *m_main;
    QLineEdit *m_find;
    QRadioButton *m_with, *m_without;
    QCheckBox *m_case, *m_regex, *m_exact, *m_dialogue, *m_comments;
    QButtonGroup *m_mode; // 0 set, 1 add, 2 subtract, 3 intersect (Aegisub)
    QComboBox *m_field, *m_action;
};

class StyleEditor : public QDialog {
    Q_OBJECT
public:
    StyleEditor(const fsb::StyleData &style, QWidget *parent);
    fsb::StyleData style() const;
    // Apply / OK hand the style here; false (e.g. a name clash) keeps the editor open
    std::function<bool(const fsb::StyleData &)> onApply;
    void accept() override;
protected:
    void resizeEvent(QResizeEvent *e) override;
private:
    void preview();
    QLineEdit *m_previewText;
    QComboBox *m_encodingBox, *m_border;
    fsb::StyleData m_style;
    QLineEdit *m_name;
    QComboBox *m_font;
    QDoubleSpinBox *m_size, *m_outline, *m_shadow, *m_scaleX, *m_scaleY, *m_angle, *m_spacing;
    QCheckBox *m_bold, *m_italic, *m_underline, *m_strike;
    QString m_colors[4];
    class QToolButton *m_colorButtons[4];
    QSpinBox *m_alpha[4], *m_ml, *m_mr, *m_mv;
    QRadioButton *m_align[9];
    QLabel *m_preview = nullptr;
};

class StyleManager : public QDialog {
    Q_OBJECT
public:
    explicit StyleManager(MainWindow *main);
    void editStyle(const QString &name);
private:
    // checks the name like Aegisub, then stores the style; index -1 = new, set to where it landed
    bool applyToFile(int &index, const fsb::StyleData &s);
    bool applyToStorage(int &index, const fsb::StyleData &s);
    // Aegisub: the block from the first to the last selected style goes to the top (0), up (1),
    // down (2) or to the bottom (3)
    void moveStyles(bool storage, int type);
    // Kainote: a style replaces one of the same name only when the user says so
    std::vector<fsb::StyleData> askOverwrite(const std::vector<fsb::StyleData> &chosen, const QStringList &taken);
    void reload();
    void loadCatalog();
    void saveCatalog();
    MainWindow *m_main;
    QComboBox *m_catalog;
    QListWidget *m_storage, *m_file;
    std::vector<fsb::StyleData> m_stored;
};

class PropertiesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PropertiesDialog(TabPanel *tab);
    void accept() override;
private:
    TabPanel *m_tab;
    std::vector<std::pair<QString, QLineEdit *>> m_fields;
    QSpinBox *m_w, *m_h, *m_lw, *m_lh;
    QComboBox *m_matrix, *m_wrap, *m_collisions;
    QCheckBox *m_scaled;
};

class HistoryDialog : public QDialog {
    Q_OBJECT
public:
    explicit HistoryDialog(TabPanel *tab);
};

class ResampleDialog : public QDialog {
    Q_OBJECT
public:
    explicit ResampleDialog(TabPanel *tab);
};

class FontCollectorDialog : public QDialog {
    Q_OBJECT
public:
    explicit FontCollectorDialog(TabPanel *tab);
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent);
};

// Autosave folder; files are "<name>.<yyyy-MM-dd-HH-mm-ss>.AUTOSAVE.ass" (Aegisub naming).
QString autosaveDir();

// Aegisub's "Open autosave file": files on the left, their versions (newest first) on the right.
class AutosaveDialog : public QDialog {
    Q_OBJECT
public:
    explicit AutosaveDialog(QWidget *parent);
    QString chosen() const;

private:
    QListWidget *m_files, *m_versions;
};

// Aegisub's spell checker: walks the misspelled words from the active line on.
class SpellCheckDialog : public QDialog {
    Q_OBJECT
public:
    SpellCheckDialog(MainWindow *main, TabPanel *tab);
    bool findNext(); // false when the whole script was checked

private:
    void showWord(int row, int start, const QString &word);
    void replace(bool all);
    MainWindow *m_main;
    QPointer<TabPanel> m_tab;
    QLineEdit *m_word, *m_with;
    QListWidget *m_suggest;
    QCheckBox *m_skipComments, *m_skipUpper;
    QSet<QString> m_ignored;
    int m_row = 0, m_pos = 0, m_start = 0, m_startRow = 0;
    bool m_wrapped = false;
};

// Aegisub's "Select fields to paste over": the Session::paste_over bits, or -1 when cancelled.
// Fields of a line as bits 1 comment .. 1024 text; `copy` (Kainote's copy columns) adds 2048 text without tags.
int chooseFields(QWidget *parent, bool copy = false);
// "&HAABBGGRR" (a style's) or "&HBBGGRR&" (a tag's) as a colour; ASS alpha 0 is opaque
QColor assColor(const QString &s);

// Aegisub's attachment manager: fonts and graphics embedded in the script.
class AttachmentsDialog : public QDialog {
    Q_OBJECT
public:
    AttachmentsDialog(QWidget *parent, TabPanel *tab);

private:
    void refresh();
    TabPanel *m_tab;
    QTreeWidget *m_list;
};

// Aegisub's styling assistant: type the start of a style name, Enter sets it on
// the active line and moves to the next one.
// Aegisub's kanji timer: groups a romaji karaoke line's syllables with the
// characters of a kanji line and writes the timing onto the kanji line.
class KanjiTimerDialog : public QDialog {
    Q_OBJECT
public:
    KanjiTimerDialog(QWidget *parent, TabPanel *tab);
    void done(int r) override;

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    int find(int from, const QString &style, int step) const;
    void reset();
    void refresh();
    void link();
    void acceptLine();
    QPointer<TabPanel> m_tab;
    rust::Box<fsb::KanjiTimer> m_kt;
    QComboBox *m_src, *m_dst;
    QCheckBox *m_interpolate;
    QLabel *m_srcView, *m_dstView;
    int m_srcRow = -1, m_dstRow = -1;
    std::vector<std::pair<size_t, QString>> m_changes;
};

class StylingAssistant : public QDialog {
    Q_OBJECT
public:
    StylingAssistant(QWidget *parent, TabPanel *tab);

private:
    void lineChanged();
    void commit(bool next);
    QPointer<TabPanel> m_tab;
    QPlainTextEdit *m_text;
    QListWidget *m_styles;
    QLineEdit *m_name;
    QCheckBox *m_seek;
};
