//! DC2's disc: the ISO 9660 image, the DATA.HD4 index of DATA.DAT, the packs inside
//! its files, extracting them all, and writing a new image with files replaced or added.

use std::fs::File;
use std::path::{Path, PathBuf};

pub mod archive;
mod cp932_fixes;
pub mod extract;
pub mod files;
pub mod iso;
pub mod layout;
pub mod rebuild;
#[cfg(any(test, feature = "test-support"))]
pub mod samples;
pub mod source;
pub use source::DiscSource;
pub mod text;
pub mod textfile;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// Data that is not what it should be.
    #[error("{0}")]
    Invalid(String),
    /// An image without an ISO 9660 volume descriptor.
    #[error("not an ISO 9660 image")]
    NotIso,
    /// A file the disc must have (DATA.HD4, DATA.DAT, a file a patch names) that it does not.
    #[error("{0} is missing from the disc")]
    Missing(String),
    /// A file that could not be read or written, by its path. The message includes the cause (it
    /// is not a separate source), so it reads whole wherever it is shown.
    #[error("cannot {action} {}: {error}", path.display())]
    File { action: &'static str, path: PathBuf, error: std::io::Error },
    #[error(transparent)]
    Io(#[from] std::io::Error),
}

impl Error {
    pub fn invalid(message: impl Into<String>) -> Error {
        Error::Invalid(message.into())
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// Names the file an io::Error is about: `fs::read(&path).with_path("read", &path)?`.
pub(crate) trait WithPath<T> {
    fn with_path(self, action: &'static str, path: &Path) -> Result<T>;
}

impl<T> WithPath<T> for std::io::Result<T> {
    fn with_path(self, action: &'static str, path: &Path) -> Result<T> {
        self.map_err(|error| Error::File { action, path: path.to_path_buf(), error })
    }
}

/// A disc image mapped into memory, read without copying.
pub struct Image {
    map: memmap2::Mmap,
}

impl Image {
    pub fn open(path: &Path) -> Result<Image> {
        if path.is_dir() {
            return Err(Error::invalid(format!("{} is a folder, not a disc image", path.display())));
        }
        let file = File::open(path).with_path("read", path)?;
        // Safety: a mapped file must not change while it is mapped. The tools only read an image
        // they map, no other process writes it meanwhile, and a build never writes the image it
        // reads (rebuild::write_disc refuses an output that is its input).
        let map = unsafe { memmap2::Mmap::map(&file).with_path("read", path)? };
        Ok(Image { map })
    }

    pub fn bytes(&self) -> &[u8] {
        &self.map
    }
}
