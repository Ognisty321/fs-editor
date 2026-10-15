// Visual typesetting tools on the video (Kainote's tool row): position
// pointer, \pos, \move, scale, Z and X/Y rotation, rectangle and vector
// \clip / \iclip, \p drawings, the position shifter and the all-tags tool.
#pragma once

#include "bridge.h"

#include <QObject>
#include <QPointF>
#include <vector>

class VideoBox;
class QPainter;
class QMouseEvent;
class QKeyEvent;
class QWidget;
class QStackedWidget;
class QToolButton;
class QComboBox;
class QDoubleSpinBox;

// A parsed ASS drawing: one command letter with its points each.
struct Drawing {
    struct Cmd {
        char type; // m n l b s p c
        std::vector<QPointF> pts;
    };
    std::vector<Cmd> cmds;
    static Drawing parse(const QString &s);
    QString toString() const;
    // flat point list (cmd, index) for hit testing
    std::vector<std::pair<int, int>> handles() const;
    QPointF &at(std::pair<int, int> h) { return cmds[h.first].pts[h.second]; }
    QRectF bounds() const;
    void translate(QPointF d);
    QPointF last() const; // the last point of the figure being drawn
};

class VisualTools : public QObject {
    Q_OBJECT
public:
    bool shown() const; // the active line is on the video frame
    enum Tool { Pointer, Pos, Move, Scale, RotZ, RotXY, RClip, VClip, Draw, Shifter, AllTags, ToolCount };
    enum VecMode { VecMove, VecLine, VecBezier, VecNewFigure, VecDelete, VecFreehand, VecInsert, VecConvert, VecSpline };

    explicit VisualTools(VideoBox *box);
    int tool() const { return m_tool; }
    void setTool(int t);
    QWidget *toolRow(QWidget *parent); // tool buttons
    QWidget *options() const; // the tool's own options, a row of its own (made by toolRow)

    void reload(); // active line changed or edited
    void paint(QPainter &p);
    bool press(QMouseEvent *e);
    bool move(QMouseEvent *e);
    bool release(QMouseEvent *e);
    bool doubleClick(QMouseEvent *e);
    bool key(QKeyEvent *e);

private:
    QPointF w(QPointF s) const; // script -> widget
    QPointF s(QPointF w) const; // widget -> script
    // allSelected: scale, rotation and clips go to every selected line (Aegisub SetSelectedOverride)
    void apply(const QString &tags, bool allSelected = false);
    void applyPos(QPointF p);
    void toggleMove();
    void applyAllTags(double value); // all-tags tool, from the texts in m_tagBase
    QString clipTag() const;
    void writeRectClip(); // m_v's rectangle to the grabbed lines, each keeping its own \iclip (Aegisub)
    void writeVector();
    // how far libass moves \p drawing points for the line's alignment
    QPointF alignShift(const Drawing &d) const;
    int hitHandle(QPointF widgetPos) const;
    QPointF vecW(QPointF pt) const; // vector point -> widget
    // drawing point (offset from \pos, unscaled) <-> script coordinates, turned by \frz around \org
    QPointF drawToScript(QPointF pt) const;
    QPointF scriptToDraw(QPointF s) const;
    // the l/b command whose segment passes near the point (t: where on it), or -1
    int hitSegment(QPointF widgetPos, double &t) const;
    // the active line is on the video's frame and not a comment: only then the tools work (Aegisub)
    void grabLines();         // the selected lines, as they are, for moveLines
    void moveLines(QPointF d); // the grabbed lines moved by d

    VideoBox *m_box;
    int m_tool = Pointer;
    int m_vecMode = VecMove;
    fsb::Visual m_v{};
    bool m_valid = false;
    qint64 m_first = 0, m_last = -1; // the frames the active line is shown on
    std::vector<size_t> m_rows;      // the lines a drag moves, and how they were
    std::vector<fsb::Visual> m_starts;
    Drawing m_vec;            // vector clip or drawing being edited
    QPointF m_drawOrigin, m_drawScale{1, 1};
    // drag state
    int m_drag = 0;           // 0 none, else tool-specific handle id
    Qt::MouseButton m_button = Qt::NoButton;
    QPointF m_pressS, m_pressW, m_mouseW;
    fsb::Visual m_start{};
    std::pair<int, int> m_vecHandle{-1, -1};
    int m_dragId = 0;
    QString m_amend;
    QStackedWidget *m_sub = nullptr;
    int m_shiftWhat = 47; // all but drawing points
    int m_scaleWhat = 5;  // scale tool: 1 width, 2 keep proportions, 4 height
    QComboBox *m_shapeBox = nullptr; // ready shapes dragged out on the video (Kainote)
    size_t m_shapeFrom = 0;          // first command of the shape being dragged
    QPointF m_shapeA;                // its press point in drawing units
    bool m_twoPoints = false; // Z rotation: the angle of a line dragged on the video
    QWidget *m_invert = nullptr;
    QComboBox *m_tagName = nullptr, *m_tagMode = nullptr;
    QDoubleSpinBox *m_tagValue = nullptr;
    std::vector<size_t> m_tagRows;
    rust::Vec<rust::String> m_tagBase; // the rows' texts when the drag started
    double m_tagStart = 0;
    std::vector<QToolButton *> m_buttons;
};
