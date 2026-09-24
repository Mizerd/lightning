//! Location: static `m.location` (MSC3488) and live beacons (MSC3672).
//!
//! Lightning has no position source (no GPS, and IP geolocation would be
//! inaccurate and an unasked-for privacy decision), so:
//!
//! * Receiving both kinds is implemented; phones (Element X, Element) send
//!   them.
//! * Sending is not. A static location from a desktop is just a map link,
//!   which a message already is, and a live share that never moves would be
//!   a false claim to everyone in the room.
//!
//! ruma stores the geo URI verbatim, so this module parses it and only the
//! numbers cross the FFI. C++ builds an OpenStreetMap https link from them;
//! the raw `geo:` string never reaches the desktop, and `UrlLauncher`'s
//! allowlist is not widened.

/// A parsed `geo:` URI.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct GeoPoint {
    pub lat: f64,
    pub lon: f64,
    /// The `;u=` uncertainty in metres, when the sender gave one.
    pub uncertainty_m: Option<f64>,
}

/// Parse `geo:<lat>,<lon>[,<alt>][;u=<metres>]` (RFC 5870). Coordinates are
/// attacker input, so out-of-range values fail and the message renders as
/// text instead of a link to a point that does not exist.
pub(crate) fn parse_geo_uri(uri: &str) -> Option<GeoPoint> {
    let trimmed = uri.trim();
    // The scheme is case-insensitive per RFC 3986.
    let rest = trimmed
        .get(..4)
        .filter(|p| p.eq_ignore_ascii_case("geo:"))
        .and_then(|_| trimmed.get(4..))?;

    // Only `u=` is read; other parameters (`crs=`, future ones) are ignored, not
    // refused, so the position survives.
    let (coords, params) = match rest.split_once(';') {
        Some((c, p)) => (c, Some(p)),
        None => (rest, None),
    };
    let mut parts = coords.split(',');
    let lat: f64 = parts.next()?.trim().parse().ok()?;
    let lon: f64 = parts.next()?.trim().parse().ok()?;
    // A third component is altitude: accepted and discarded.
    if !lat.is_finite() || !lon.is_finite() {
        return None;
    }
    if !(-90.0..=90.0).contains(&lat) || !(-180.0..=180.0).contains(&lon) {
        return None;
    }
    let uncertainty_m = params.and_then(|p| {
        p.split(';')
            // RFC 5870 parameter names are case-insensitive.
            .find_map(|kv| {
                let kv = kv.trim();
                kv.get(..2)
                    .filter(|p| p.eq_ignore_ascii_case("u="))
                    .and_then(|_| kv.get(2..))
            })
            .and_then(|v| v.trim().parse::<f64>().ok())
            .filter(|v| v.is_finite() && *v >= 0.0)
    });
    Some(GeoPoint { lat, lon, uncertainty_m })
}

/// Put a location's fields on a timeline event. An unparseable `geo` leaves
/// `locationLat`/`locationLon` absent, so the UI renders text; zero would be
/// a confident point in the Atlantic.
pub(crate) fn fill_location(
    out: &mut serde_json::Value,
    geo: &str,
    body: &str,
    description: Option<&str>,
    asset: Option<&str>,
) {
    out["msgtype"] = "location".into();
    // The sender's own words stay the body, so search, notifications and
    // previews read sensibly without knowing about locations.
    out["body"] = body.to_owned().into();
    if let Some(point) = parse_geo_uri(geo) {
        out["locationLat"] = point.lat.into();
        out["locationLon"] = point.lon.into();
        if let Some(u) = point.uncertainty_m {
            out["locationUncertaintyM"] = u.into();
        }
    }
    if let Some(d) = description.filter(|d| !d.is_empty()) {
        out["locationDescription"] = d.to_owned().into();
    }
    if let Some(a) = asset.filter(|a| !a.is_empty()) {
        // "m.self" (the sender's position) or "m.pin" (a place); worded
        // differently on screen.
        out["locationAsset"] = a.to_owned().into();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_geo_uri_is_parsed_with_its_optional_parts() {
        let p = parse_geo_uri("geo:51.5008,-0.1247").unwrap();
        assert!((p.lat - 51.5008).abs() < 1e-9);
        assert!((p.lon - (-0.1247)).abs() < 1e-9);
        assert_eq!(p.uncertainty_m, None);

        // Uncertainty, and an altitude that is discarded.
        let p = parse_geo_uri("geo:51.5008,-0.1247,15;u=35").unwrap();
        assert_eq!(p.uncertainty_m, Some(35.0));
        // RFC 5870 parameter names are case-insensitive.
        let p = parse_geo_uri("geo:1.0,2.0;U=35").unwrap();
        assert_eq!(p.uncertainty_m, Some(35.0));

        // An unfamiliar parameter must not lose the position.
        let p = parse_geo_uri("geo:1.0,2.0;crs=wgs84;u=10").unwrap();
        assert_eq!(p.uncertainty_m, Some(10.0));

        // The scheme is case-insensitive (RFC 3986).
        assert!(parse_geo_uri("GEO:1.0,2.0").is_some());
    }

    #[test]
    fn a_point_that_is_not_on_earth_is_refused() {
        // Attacker input: a refusal renders as text rather than a link to nowhere.
        assert!(parse_geo_uri("geo:91.0,0.0").is_none());
        assert!(parse_geo_uri("geo:-91.0,0.0").is_none());
        assert!(parse_geo_uri("geo:0.0,181.0").is_none());
        assert!(parse_geo_uri("geo:0.0,-181.0").is_none());
        assert!(parse_geo_uri("geo:NaN,0.0").is_none());
        assert!(parse_geo_uri("geo:inf,0.0").is_none());

        // Not a geo URI; `https:` must not be mistaken for one.
        assert!(parse_geo_uri("https://example.org/1,2").is_none());
        assert!(parse_geo_uri("geo:").is_none());
        assert!(parse_geo_uri("geo:51.5008").is_none());
        assert!(parse_geo_uri("").is_none());
        assert!(parse_geo_uri("geo:abc,def").is_none());
    }

    #[test]
    fn an_unparseable_uri_leaves_the_coordinates_absent() {
        // Absent, not zero: absence selects plain-text rendering.
        let mut out = serde_json::json!({});
        fill_location(&mut out, "geo:999,999", "Somewhere", None, None);
        assert_eq!(out["msgtype"], "location");
        assert_eq!(out["body"], "Somewhere");
        assert!(out.get("locationLat").is_none());
        assert!(out.get("locationLon").is_none());

        let mut ok = serde_json::json!({});
        fill_location(&mut ok, "geo:1.5,2.5;u=8", "Here", Some("The pub"),
                      Some("m.pin"));
        assert_eq!(ok["locationLat"], 1.5);
        assert_eq!(ok["locationLon"], 2.5);
        assert_eq!(ok["locationUncertaintyM"], 8.0);
        assert_eq!(ok["locationDescription"], "The pub");
        assert_eq!(ok["locationAsset"], "m.pin");
    }
}
