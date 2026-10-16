//! Aegisub's call tips (libaegisub's GetCalltip): the prototype of the override tag the cursor is
//! in, and which of its parameters the cursor is on.

use crate::syntax::{Tok, tokenize};

/// Aegisub's prototypes; a tag's overloads follow each other, fewest arguments first.
const PROTOS: &[(&str, &str)] = &[
    ("K", "\\KDuration"),
    ("a", "\\aAlignment"),
    ("b", "\\bWeight"),
    ("c", "\\cColour"),
    ("i", "\\i1/0"),
    ("k", "\\kDuration"),
    ("p", "\\pExponent"),
    ("q", "\\qWrap Style"),
    ("r", "\\rStyle"),
    ("s", "\\s1/0"),
    ("t", "\\t(Acceleration,Tags)"),
    ("t", "\\t(Start Time,End Time,Tags)"),
    ("t", "\\t(Start Time,End Time,Acceleration,Tags)"),
    ("u", "\\u1/0"),
    ("1a", "\\1aAlpha"),
    ("1c", "\\1cColour"),
    ("2a", "\\2aAlpha"),
    ("2c", "\\2cColour"),
    ("3a", "\\3aAlpha"),
    ("3c", "\\3cColour"),
    ("4a", "\\4aAlpha"),
    ("4c", "\\4cColour"),
    ("an", "\\anAlignment"),
    ("be", "\\beStrength"),
    ("fe", "\\feEncoding"),
    ("fn", "\\fnFont Name"),
    ("fr", "\\frAngle"),
    ("fs", "\\fsFont Size"),
    ("kf", "\\kfDuration"),
    ("ko", "\\koDuration"),
    ("fax", "\\faxFactor"),
    ("fay", "\\fayFactor"),
    ("frx", "\\frxAngle"),
    ("fry", "\\fryAngle"),
    ("frz", "\\frzAngle"),
    ("fsp", "\\fspSpacing"),
    ("org", "\\org(X,Y)"),
    ("pbo", "\\pboOffset"),
    ("pos", "\\pos(X,Y)"),
    ("blur", "\\blurStrength"),
    ("bord", "\\bordWidth"),
    ("clip", "\\clip(Command)"),
    ("clip", "\\clip(Scale,Command)"),
    ("clip", "\\clip(X1,Y1,X2,Y2)"),
    ("fad", "\\fad(Start Time,End Time)"),
    (
        "fade",
        "\\fade(Start Alpha,Middle Alpha,End Alpha,Start In,End In,Start Out,End Out)",
    ),
    ("fscx", "\\fscxScale"),
    ("fscy", "\\fscyScale"),
    ("move", "\\move(X1,Y1,X2,Y2)"),
    ("move", "\\move(X1,Y1,X2,Y2,Start Time,End Time)"),
    ("shad", "\\shadDepth"),
    ("alpha", "\\alphaAlpha"),
    ("iclip", "\\iclip(Command)"),
    ("iclip", "\\iclip(Scale,Command)"),
    ("iclip", "\\iclip(X1,Y1,X2,Y2)"),
    ("xbord", "\\xbordWidth"),
    ("xshad", "\\xshadDepth"),
    ("ybord", "\\ybordWidth"),
    ("yshad", "\\yshadDepth"),
];

#[derive(Debug, PartialEq, Eq)]
pub struct Calltip {
    /// The prototype, e.g. `\pos(X,Y)`.
    pub text: &'static str,
    /// The parameter the cursor is on, as a byte range of `text`.
    pub highlight: std::ops::Range<usize>,
    /// Where the tag's name starts in the line, in bytes.
    pub tag_pos: usize,
}

/// The call tip at byte `pos` of a line's text, None outside a known tag or past its last parameter.
pub fn calltip(text: &str, pos: usize) -> Option<Calltip> {
    let tokens = tokenize(text, false);
    // the tokens starting before the cursor: the last tag name in an override block still open,
    // and the separators since it
    let (mut idx, mut left, mut at) = (0, pos, 0);
    let (mut tag, mut commas) = (None, 0);
    while idx < tokens.len() && left > 0 {
        match tokens[idx].ty {
            Tok::Comment | Tok::OvrEnd => tag = None,
            Tok::TagName => {
                tag = Some((at, tokens[idx].len));
                commas = 0;
            }
            Tok::ArgSep => commas += 1,
            _ => {}
        }
        left -= left.min(tokens[idx].len);
        at += tokens[idx].len;
        idx += 1;
    }
    let (tag_pos, len) = tag?;
    let name = &text[tag_pos..tag_pos + len];
    let mut overloads = PROTOS.iter().filter(|p| p.0 == name).map(|p| p.1);
    let mut proto = overloads.next()?;
    // the overload with the fewest arguments that has as many as the tag (Aegisub skips the token
    // after the cursor's, missing a separator there)
    let mut args = commas + 1;
    for t in &tokens[idx..] {
        match t.ty {
            Tok::ArgSep => args += 1,
            Tok::Arg | Tok::Whitespace => {}
            _ => break,
        }
    }
    for next in overloads {
        if args <= proto.matches(',').count() + 1 {
            break;
        }
        proto = next;
    }
    let first = len + 1; // the slash and the name
    let highlight = if proto.as_bytes()[first] != b'(' {
        first..proto.len()
    } else {
        let mut start = first + 1;
        for _ in 0..commas {
            start += proto[start..].find(',')? + 1;
        }
        start
            ..proto[start..]
                .find(',')
                .map_or(proto.len() - 1, |e| start + e)
    };
    Some(Calltip {
        text: proto,
        highlight,
        tag_pos,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The highlighted parameter with the cursor just before `at`'s first occurrence + `shift`.
    fn tip(text: &str, at: &str, shift: usize) -> Option<(&'static str, &'static str)> {
        let pos = text.find(at).unwrap() + shift;
        calltip(text, pos).map(|c| (c.text, &c.text[c.highlight]))
    }

    #[test]
    fn parameters_under_the_cursor() {
        let line = "{\\pos(10,20)\\bord2\\fnArial}Text";
        assert_eq!(tip(line, "10", 1), Some(("\\pos(X,Y)", "X")));
        assert_eq!(tip(line, "20", 1), Some(("\\pos(X,Y)", "Y")));
        assert_eq!(tip(line, "2\\", 1), Some(("\\bordWidth", "Width")));
        assert_eq!(tip(line, "Arial", 2), Some(("\\fnFont Name", "Font Name")));
        assert_eq!(
            calltip(line, line.find("pos").unwrap()).map(|c| c.tag_pos),
            None
        ); // just after the slash
        assert_eq!(
            calltip(line, line.find("pos").unwrap() + 1).map(|c| c.tag_pos),
            Some(2)
        );
        assert_eq!(tip(line, "Text", 2), None);
    }

    #[test]
    fn overloads_by_their_arguments() {
        assert_eq!(
            tip("{\\move(1,2,3,4)}", "1", 1),
            Some(("\\move(X1,Y1,X2,Y2)", "X1"))
        );
        assert_eq!(
            tip("{\\move(1,2,3,4,5,6)}", "5", 1),
            Some(("\\move(X1,Y1,X2,Y2,Start Time,End Time)", "Start Time"))
        );
        assert_eq!(
            tip("{\\t(0,100,\\frz30)}", "100", 1),
            Some(("\\t(Start Time,End Time,Tags)", "End Time"))
        );
        assert_eq!(
            tip("{\\t(0,100,2,\\frz30)}", "0", 1),
            Some(("\\t(Start Time,End Time,Acceleration,Tags)", "Start Time"))
        );
        // a tag inside \t has its own
        assert_eq!(
            tip("{\\t(0,100,\\frz30)}", "30", 1),
            Some(("\\frzAngle", "Angle"))
        );
        assert_eq!(tip("{\\fad(1,2,3)}", "3", 1), None); // past the last parameter
        assert_eq!(tip("{\\xyz(1)}", "1", 1), None);
    }
}
