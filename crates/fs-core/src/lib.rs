//! FS Editor core: document model, lossless ASS parser/writer, override tags,
//! timing (including VFR), undo history and editing operations.

pub mod ass;
pub mod attachments;
pub mod color;
pub mod fonts;
pub mod history;
pub mod karaoke;
pub mod keyframes;
pub mod model;
pub mod ops;
pub mod tag_adjust;
pub mod tags;
pub mod text;
pub mod time;
pub mod tlmode;
pub mod vfr;

pub use ass::{Document, EventEntry, EventId, SectionKind};
pub use color::Color;
pub use model::{AssVersion, Event, EventKind, Style};
pub use time::Time;
