//! DC2's scripts: the .stb format, its text assembly, the native tables, decompiling
//! to C and compiling back, and the tools a mod is built with.

// The modules by area. Those only the crate itself uses are pub(crate); the others are what
// the dc2tk binary and the integration tests (tests/) use.

// The program tree, the .stb format and its assembly, the native tables, the test VM
#[macro_use]
pub mod node;
pub mod asm;
pub(crate) mod fingerprint;
pub mod natives;
pub mod stack;
pub mod stb;
pub mod vm;

// The C-like language: parsing, the SDK's headers, lowering and compiling, writing source
pub mod compile;
pub(crate) mod composite;
pub(crate) mod cpp;
pub(crate) mod ctype;
pub(crate) mod lower;
pub mod parse;
pub mod sdk;
pub mod source;

// Decompiling a disc: the program tree from a script, named and presented with the definitions
pub mod assets;
pub mod coverage;
pub mod decompile;
pub mod decompiler;
pub mod definitions;
pub mod disc;
pub mod learn;
pub mod library;
pub mod messages;
pub mod names;
pub mod notes;
pub mod slotflow;
pub mod slots;
pub mod soundpass;
pub mod villagers;

// A decompiled tree, read as text: its record, names, flags, constants and cross-references
pub mod constants;
pub mod flags;
pub mod index;
pub mod loads;
pub mod maps;
pub mod pathnames;
pub mod tree;
pub mod xref;

// Mods, and building an image with them
pub mod additions;
pub mod build;
pub mod capture;
pub mod datatables;
pub mod discfiles;
pub mod editor;
pub(crate) mod itemtext;
pub mod manifest;
pub mod mapedit;
pub mod mapscript;
pub mod maptable;
pub mod memory;
pub mod mods;
pub mod numbers;
pub mod package;
pub mod patches;
pub mod replace;
pub mod semver;
pub mod sets;
pub mod slim;
pub mod startat;

// Paths as the tools write them, and walking folders
pub mod paths;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0}")]
    Invalid(String),
    /// What is wrong with a statement of a source file, and its line there.
    #[error("line {line}: {error}")]
    At { line: i64, error: Box<Error> },
    /// An error with what was being done when it happened ("texture:tex02 (mod fish) at ...").
    #[error("{what}: {error}")]
    Context { what: String, error: Box<Error> },
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error(transparent)]
    Disc(#[from] dc2tk_disc::Error),
}

impl Error {
    pub fn invalid(message: impl Into<String>) -> Error {
        Error::Invalid(message.into())
    }

    /// The error of a statement on a line (a node's "line", when it has one): an error of the
    /// source given the line, unless it has the line of a statement inside this one already.
    pub fn at(self, line: &node::Node) -> Error {
        match (self, line.as_i64()) {
            (error @ Error::Invalid(_), Some(line)) => Error::At { line, error: Box::new(error) },
            (error, _) => error,
        }
    }

    /// Whether the error is one of the input (a source file, a tree) rather than of reading or
    /// writing a file.
    pub fn is_input(&self) -> bool {
        match self {
            Error::Invalid(_) | Error::At { .. } => true,
            Error::Context { error, .. } => error.is_input(),
            Error::Io(_) | Error::Disc(_) => false,
        }
    }
}

impl From<dc2tk_assets::Error> for Error {
    fn from(error: dc2tk_assets::Error) -> Error {
        match error {
            dc2tk_assets::Error::Invalid(message) => Error::Invalid(message),
            dc2tk_assets::Error::Io(error) => Error::Io(error),
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// An error said with what was being done when it happened: `.context("map:s17 (mod fish)")`
/// makes it an Error::Context, shown as "map:s17 (mod fish): <the error>", which keeps whether it
/// was one of the input.
pub trait Context<T> {
    fn context(self, what: impl std::fmt::Display) -> Result<T>;
    /// context, with the description made only on an error.
    fn with_context<D: std::fmt::Display>(self, what: impl FnOnce() -> D) -> Result<T>;
}

impl<T, E: Into<Error>> Context<T> for std::result::Result<T, E> {
    fn context(self, what: impl std::fmt::Display) -> Result<T> {
        self.map_err(|error| Error::Context { what: what.to_string(), error: Box::new(error.into()) })
    }

    fn with_context<D: std::fmt::Display>(self, what: impl FnOnce() -> D) -> Result<T> {
        self.map_err(|error| Error::Context { what: what().to_string(), error: Box::new(error.into()) })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_statements_error_has_the_line_of_the_innermost_statement() {
        let inner = Error::invalid("unknown variable x").at(&node::Node::Int(6));
        assert!(inner.is_input());
        assert_eq!(inner.at(&node::Node::Int(5)).to_string(), "line 6: unknown variable x");
        assert_eq!(Error::invalid("no main").at(&node::Node::Null).to_string(), "no main");
    }

    #[test]
    fn an_error_in_context_says_what_was_being_done_and_keeps_its_cause() {
        let wrap = |error: Error| Error::Context { what: "texture:tex02 (mod fish)".into(), error: Box::new(error) };
        let input = wrap(Error::invalid("no picture 3"));
        assert_eq!(input.to_string(), "texture:tex02 (mod fish): no picture 3");
        assert!(input.is_input());
        let io = wrap(Error::Io(std::io::Error::new(std::io::ErrorKind::NotFound, "gone")));
        assert!(!io.is_input());
        assert!(matches!(io, Error::Context { error, .. } if matches!(*error, Error::Io(_))));
    }
}
