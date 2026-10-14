// Visual typesetting tools on the video (Kainote's tool row): position
// pointer, \pos, \move, scale, Z and X/Y rotation, rectangle and vector
// \clip / \iclip, \p drawings and the position shifter.
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
};

class VisualTools : public QObject {
    Q_OBJECT
public:
    enum Tool { Pointer, Pos, Move, Scale, RotZ, RotXY, RClip, VClip, Draw, Shifter, ToolCount };
    enum VecMode { VecMove, VecLine, VecBezier, VecNewFigure, VecDelete };

    explicit VisualTools(VideoBox *box);
    int tool() const { return m_tool; }
    void setTool(int t);
    QWidget *toolRow(QWidget *parent); // tool buttons + sub-toolbar

    void reload(); // active line changed or edited
    void paint(QPainter &p);
    bool press(QMouseEvent *e);
    bool move(QMouseEvent *e);
    bool release(QMouseEvent *e);
    bool key(QKeyEvent *e);

private:
    QPointF w(QPointF s) const; // script -> widget
    QPointF s(QPointF w) const; // widget -> script
    // allSelected: scale, rotation and clips go to every selected line (Aegisub SetSelectedOverride)
    void apply(const QString &tags, bool allSelected = false);
    void applyPos(QPointF p);
    void toggleMove();
    QString clipTag() const;
    void writeVector();
    // \p drawing origin and per-unit scale in script coordinates
    void drawingFrame(QPointF &origin, QPointF &scale) const;
    int hitHandle(QPointF widgetPos) const;

    VideoBox *m_box;
    int m_tool = Pointer;
    int m_vecMode = VecMove;
    fsb::Visual m_v{};
    bool m_valid = false;
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
    QWidget *m_invert = nullptr;
    std::vector<QToolButton *> m_buttons;
};
