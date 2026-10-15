// The edit box (Kainote layout): tag bar, info row, TL original editor,
// text editor with tag highlighting and the fields row. Edits are applied
// live; consecutive edits of one field merge into one undo step.
#pragma once

#include "bridge.h"

#include <QElapsedTimer>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QWidget>

#include <unordered_map>

class TabPanel;
class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QRadioButton;
class QToolButton;
class QHBoxLayout;
class QCompleter;
class QRegularExpression;

// Start/End/Duration field: h:mm:ss.cc in overwrite mode, or a frame number.
// Hunspell spell checking shared by every text box (settings spell/enabled, spell/dir, spell/lang).
namespace spell {
fsb::Speller *get(); // nullptr when disabled or no dictionary could be loaded
void reload();       // after a settings change: reload the dictionary, re-highlight
QString dir();
QStringList languages();
void addWord(const QString &word); // to the user dictionary of the loaded language
int generation();                  // changes on every reload, for caches of results
} // namespace spell

// Character counter limits from Settings (subs/maxCps, subs/maxLineLength, subs/maxLines), and what
// characters per second counts (subs/cpsIgnore*, applied by reload()).
namespace limits {
int cps();
bool wrapsOver(const QString &wraps); // "a/b/c" line lengths: too many lines or one too long
void reload();                        // after a settings change
} // namespace limits

class TimeEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit TimeEdit(QWidget *parent = nullptr);
    void setFrames(bool on);
    void setMs(qint64 ms, qint64 frame);
    qint64 ms() const;      // -1 when invalid
    qint64 frame() const;
signals:
    void edited();
protected:
    void wheelEvent(QWheelEvent *) override;
private:
    bool m_frames = false;
};

class SubsTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit SubsTextEdit(QWidget *parent = nullptr);
    QString text() const { return toPlainText(); }
    void setText(const QString &t);
    void insertTag(const QString &tag);
    void toggleTag(const QString &name, bool styleOn);
    // Capture 1 of the last `re` among the tags in effect at `pos` (Aegisub's), empty if none
    QString tagValue(const QRegularExpression &re, int pos) const;
signals:
    void keyCommand(int key, Qt::KeyboardModifiers mods);
    void splitRequested(qint64 atMs); // -1 estimate, -2 keep both times, else at that time
    void revertRequested();
    void insertOriginalRequested();
protected:
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void wheelEvent(QWheelEvent *) override;
private:
    std::pair<int, int> blockAt(int pos) const;
    QCompleter *m_completer;
};

class EditBox : public QWidget {
    Q_OBJECT
public:
    explicit EditBox(TabPanel *tab);
    void load(int row);
    void reload();
    void focusText();
    void selectText(int from, int length, bool original = false); // UTF-16 positions in the line text
    std::pair<int, int> textSelection(bool original = false) const; // start, end
    int row() const { return m_row; }
    bool framesMode() const { return m_framesMode; } // the grid shows frames too (Aegisub)

protected:
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void commit(const char *field);
    void showTimes(const fsb::Row &r, const QString &skip);
    void updateInfo(const fsb::Row &r);
    void rebuildTagButtons();
    void onKey(int key, Qt::KeyboardModifiers mods);
    void nextLine();
    void splitAtCursor(qint64 atMs);
    void setColorTag(int which);
    void toggleTag(const QString &name);
    fsb::StyleData lineStyle() const;
    void configureTagButton(int index);
    void wrapFields();

    TabPanel *m_tab;
    int m_row = -1;
    QString m_initial; // the text when the line became active (Aegisub's edit/revert)
    // the times of each selected line before the time fields changed them (Aegisub's initial_times)
    std::unordered_map<size_t, std::pair<qint64, qint64>> m_initialTimes;
    // a new undo step for typing after a cursor jump, a switch between typing and deleting or a
    // 30 s pause (Scintilla's and Aegisub's)
    int m_undoStep = 0, m_editKind = 0, m_editPos = -1;
    QElapsedTimer m_lastEdit;
    bool m_loading = false;
    bool m_framesMode = false;

    QHBoxLayout *m_tagBar, *m_userTags;
    QComboBox *m_an;
    QLabel *m_lineLabel, *m_wrapsLabel, *m_cpsLabel;
    QLabel *m_textInfo; // Length / Lines / Ln / Col / Sel / Ch under the text
    void updateTextInfo();
    QCheckBox *m_tlMode;
    QRadioButton *m_timeRadio, *m_frameRadio;
    QWidget *m_tlBox;
    SubsTextEdit *m_original, *m_text;
    QCheckBox *m_hideOriginal, *m_doubtful, *m_movingTags;
    QCheckBox *m_comment;
    QSpinBox *m_layer, *m_ml, *m_mr, *m_mv;
    TimeEdit *m_start, *m_end, *m_duration;
    QComboBox *m_style, *m_actor, *m_effect;
    QToolButton *m_styleEdit;
    QWidget *m_fields1, *m_fields2;
    QHBoxLayout *m_row1, *m_row2;
    QList<QWidget *> m_wrapWidgets;
};
