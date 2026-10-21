use std::cmp::Ordering;

use crate::{Error, Result};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Version {
    pub major: u64,
    pub minor: u64,
    pub patch: u64,
}

impl std::fmt::Display for Version {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(out, "{}.{}.{}", self.major, self.minor, self.patch)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Operator {
    Exact,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
    Caret,
    Tilde,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Comparator {
    operator: Operator,
    version: Version,
    given: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Range {
    text: String,
    comparators: Vec<Comparator>,
}

impl std::fmt::Display for Range {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        out.write_str(&self.text)
    }
}

fn number(part: &str, text: &str) -> Result<u64> {
    if part.is_empty() || !part.bytes().all(|byte| byte.is_ascii_digit()) || (part.len() > 1 && part.starts_with('0')) {
        return Err(Error::invalid(format!("{text:?} is not a version: write MAJOR.MINOR.PATCH, as 1.2.0")));
    }
    part.parse().map_err(|_| Error::invalid(format!("{text:?} is not a version: a number is too large")))
}

pub fn parse_version(text: &str) -> Result<Version> {
    let parts: Vec<&str> = text.split('.').collect();
    if parts.len() != 3 {
        return Err(Error::invalid(format!("{text:?} is not a version: write MAJOR.MINOR.PATCH, as 1.2.0")));
    }
    Ok(Version { major: number(parts[0], text)?, minor: number(parts[1], text)?, patch: number(parts[2], text)? })
}

fn partial(text: &str, whole: &str) -> Result<(Version, usize)> {
    let parts: Vec<&str> = text.split('.').collect();
    if parts.is_empty() || parts.len() > 3 {
        return Err(Error::invalid(format!("{whole:?} is not a version range")));
    }
    let mut numbers = [0u64; 3];
    for (at, part) in parts.iter().enumerate() {
        numbers[at] = number(part, whole)?;
    }
    Ok((Version { major: numbers[0], minor: numbers[1], patch: numbers[2] }, parts.len()))
}

fn comparator(text: &str, whole: &str) -> Result<Comparator> {
    let operators = [
        (">=", Operator::GreaterEqual),
        ("<=", Operator::LessEqual),
        (">", Operator::Greater),
        ("<", Operator::Less),
        ("=", Operator::Exact),
        ("^", Operator::Caret),
        ("~", Operator::Tilde),
    ];
    let (operator, rest) = operators
        .iter()
        .find_map(|(prefix, operator)| text.strip_prefix(prefix).map(|rest| (*operator, rest.trim())))
        .unwrap_or((Operator::Caret, text));
    let (version, given) = partial(rest, whole)?;
    Ok(Comparator { operator, version, given })
}

pub fn parse_range(text: &str) -> Result<Range> {
    let trimmed = text.trim();
    if trimmed == "*" {
        return Ok(Range { text: trimmed.to_string(), comparators: Vec::new() });
    }
    let comparators =
        trimmed.split(',').map(|part| comparator(part.trim(), text)).collect::<Result<Vec<_>>>().map_err(|_| {
            Error::invalid(format!(
                "{text:?} is not a version range: write \"*\", \"^1.2\", \"~1.2.3\", \">=1.0, <2.0\" or \"=1.2.3\""
            ))
        })?;
    Ok(Range { text: trimmed.to_string(), comparators })
}

fn upper_bound(comparator: &Comparator) -> Version {
    let Version { major, minor, patch } = comparator.version;
    match (comparator.operator, comparator.given) {
        (Operator::Caret, _) if major > 0 || comparator.given == 1 => Version { major: major + 1, minor: 0, patch: 0 },
        (Operator::Caret, _) if minor > 0 || comparator.given == 2 => Version { major, minor: minor + 1, patch: 0 },
        (Operator::Caret, _) => Version { major, minor, patch: patch + 1 },
        (Operator::Tilde, 1) => Version { major: major + 1, minor: 0, patch: 0 },
        (Operator::Tilde, _) => Version { major, minor: minor + 1, patch: 0 },
        (_, 1) => Version { major: major + 1, minor: 0, patch: 0 },
        (_, 2) => Version { major, minor: minor + 1, patch: 0 },
        _ => Version { major, minor, patch: patch + 1 },
    }
}

fn admits(comparator: &Comparator, version: Version) -> bool {
    let low = comparator.version;
    match comparator.operator {
        Operator::Caret | Operator::Tilde => version >= low && version < upper_bound(comparator),
        Operator::Exact => version >= low && version < upper_bound(comparator),
        Operator::Greater if comparator.given == 3 => version > low,
        Operator::Greater => version >= upper_bound(comparator),
        Operator::GreaterEqual => version >= low,
        Operator::Less => version.cmp(&low) == Ordering::Less,
        Operator::LessEqual if comparator.given == 3 => version <= low,
        Operator::LessEqual => version < upper_bound(comparator),
    }
}

pub fn matches(range: &Range, version: Version) -> bool {
    range.comparators.iter().all(|comparator| admits(comparator, version))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ok(range: &str, version: &str) -> bool {
        matches(&parse_range(range).unwrap(), parse_version(version).unwrap())
    }

    #[test]
    fn caret_allows_changes_that_keep_the_leftmost_nonzero_part() {
        assert!(ok("^1.2", "1.9.0"));
        assert!(!ok("^1.2", "2.0.0"));
        assert!(!ok("^1.2", "1.1.9"));
        assert!(ok("^0.3.1", "0.3.9"));
        assert!(!ok("^0.3.1", "0.4.0"));
        assert!(ok("^0.0.3", "0.0.3"));
        assert!(!ok("^0.0.3", "0.0.4"));
        assert!(ok("1.2", "1.4.0"));
    }

    #[test]
    fn tilde_allows_patch_changes() {
        assert!(ok("~1.2.3", "1.2.9"));
        assert!(!ok("~1.2.3", "1.3.0"));
        assert!(ok("~1", "1.9.9"));
    }

    #[test]
    fn comparisons_join_with_commas() {
        assert!(ok(">=1.0, <2.0", "1.5.0"));
        assert!(!ok(">=1.0, <2.0", "2.0.0"));
        assert!(ok("=1.2.3", "1.2.3"));
        assert!(!ok("=1.2.3", "1.2.4"));
        assert!(ok("=1.2", "1.2.7"));
        assert!(ok("*", "0.0.1"));
        assert!(ok(">1.2", "1.3.0"));
        assert!(!ok(">1.2", "1.2.5"));
        assert!(ok("<=1.2", "1.2.9"));
    }

    #[test]
    fn versions_are_three_plain_numbers() {
        assert!(parse_version("1.2.0").is_ok());
        assert!(parse_version("1.2").is_err());
        assert!(parse_version("01.2.0").is_err());
        assert!(parse_version("1.2.0-beta").is_err());
        assert!(parse_range("^x").is_err());
        assert!(parse_range("").is_err());
    }
}
