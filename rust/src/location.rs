//! Location: static `m.location` (MSC3488) and live beacons (MSC3672).
//!
//! # What a desktop can honestly do
//!
//! Lightning has no position source. There is no GPS, and deriving one from
//! an IP address would be inaccurate AND a privacy decision nobody asked
//! for. So:
//!
//! * RECEIVING both kinds is implemented in full. It needs no position.
//! * SENDING a static location is implemented: the user supplies the point,
//!   which is the ordinary "the restaurant is here" case.
//! * SENDING a LIVE location is deliberately NOT implemented.
//!   `Room::send_location_beacon` exists to be called repeatedly with new
//!   positions, and a live share that never moves is a lie told to everyone
//!   in the room under a banner that says otherwise. A button for it would
//!   be worse than its absence.
//!
//! # Parsing is ours
//!
//! ruma stores a geo URI verbatim and parses nothing — `LocationContent::new`
//! takes a `String`. So this module parses it, and the NUMBERS are what cross
//! the FFI. The C++ side builds an `https://www.openstreetmap.org/...` link
//! from them; the raw `geo:` string never reaches the desktop, and
//! `UrlLauncher`'s allowlist (http/https/mailto) is not widened for it.

/// A parsed `geo:` URI.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct GeoPoint {
    pub lat: f64,
    pub lon: f64,
    /// The `;u=` uncertainty in metres, when the sender gave one.
    pub uncertainty_m: Option<f64>,
}

/// Parse `geo:<lat>,<lon>[,<alt>][;u=<metres>]` (RFC 5870).
///
/// THE COORDINATES ARE ATTACKER INPUT — a geo URI is a field of a message
/// anyone can send — so the range check is not tidiness. An out-of-range
/// pair fails here and the message renders as plain text, rather than
/// becoming a map link to a point that does not exist.
pub(crate) fn parse_geo_uri(uri: &str) -> Option<GeoPoint> {
    let trimmed = uri.trim();
    // The scheme is case-insensitive per RFC 3986.
    let rest = trimmed
        .get(..4)
        .filter(|p| p.eq_ignore_ascii_case("geo:"))
        .and_then(|_| trimmed.get(4..))?;

    // Parameters follow a `;`. Only `u=` is read; anything else (`crs=`, and
    // whatever a future client adds) is IGNORED rather than refused, so an
    // unfamiliar parameter does not lose the position.
    let (coords, params) = match rest.split_once(';') {
        Some((c, p)) => (c, Some(p)),
        None => (rest, None),
    };
    let mut parts = coords.split(',');
    let lat: f64 = parts.next()?.trim().parse().ok()?;
    let lon: f64 = parts.next()?.trim().parse().ok()?;
    // A third component is the altitude: accepted and discarded. Nothing
    // here shows it, and refusing the URI over it would lose the position.
    if !lat.is_finite() || !lon.is_finite() {
        return None;
    }
    if !(-90.0..=90.0).contains(&lat) || !(-180.0..=180.0).contains(&lon) {
        return None;
    }
    let uncertainty_m = params.and_then(|p| {
        p.split(';')
            // Case-insensitive: RFC 5870 parameter names are, and `U=35`
            // silently losing the accuracy is the kind of thing nobody
            // reports because the position still works.
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

/// Build a `geo:` URI from coordinates, for sending. `None` when they are
/// not a real point on Earth.
///
/// Six decimal places is about 0.1 m at the equator — far finer than any
/// desktop user's input — and stopping there keeps a hand-entered coordinate
/// from being published with fifteen digits of false precision.
pub(crate) fn geo_uri(lat: f64, lon: f64) -> Option<String> {
    if !lat.is_finite() || !lon.is_finite() {
        return None;
    }
    if !(-90.0..=90.0).contains(&lat) || !(-180.0..=180.0).contains(&lon) {
        return None;
    }
    Some(format!("geo:{lat:.6},{lon:.6}"))
}

/// Put a location's fields on a timeline event.
///
/// `geo` that does not parse leaves `locationLat`/`locationLon` ABSENT. The
/// absence is what tells the UI to render text rather than a map link — a
/// zero would be a point in the Atlantic, confidently wrong.
pub(crate) fn fill_location(
    out: &mut serde_json::Value,
    geo: &str,
    body: &str,
    description: Option<&str>,
    asset: Option<&str>,
) {
    out["msgtype"] = "location".into();
    // The sender's own words — "Big Ben, London" — kept as the BODY so every
    // existing surface (search, notifications, the room-list preview) reads
    // sensibly without knowing what a location is.
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
        // "m.self" (the sender's own position) or "m.pin" (a place they are
        // pointing at). Different sentences on screen, so it crosses.
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

        // Uncertainty, and an altitude that is accepted and discarded.
        let p = parse_geo_uri("geo:51.5008,-0.1247,15;u=35").unwrap();
        assert_eq!(p.uncertainty_m, Some(35.0));
        // RFC 5870 parameter names are case-insensitive.
        let p = parse_geo_uri("geo:1.0,2.0;U=35").unwrap();
        assert_eq!(p.uncertainty_m, Some(35.0));

        // An unfamiliar parameter must not lose the position — a future
        // client adding `crs=` would otherwise make its messages unreadable.
        let p = parse_geo_uri("geo:1.0,2.0;crs=wgs84;u=10").unwrap();
        assert_eq!(p.uncertainty_m, Some(10.0));

        // The scheme is case-insensitive (RFC 3986).
        assert!(parse_geo_uri("GEO:1.0,2.0").is_some());
    }

    #[test]
    fn a_point_that_is_not_on_earth_is_refused() {
        // These are ATTACKER INPUT: a geo URI is a field of a message anyone
        // can send. A refusal renders the message as text; accepting one
        // would produce a map link to a place that does not exist.
        assert!(parse_geo_uri("geo:91.0,0.0").is_none());
        assert!(parse_geo_uri("geo:-91.0,0.0").is_none());
        assert!(parse_geo_uri("geo:0.0,181.0").is_none());
        assert!(parse_geo_uri("geo:0.0,-181.0").is_none());
        assert!(parse_geo_uri("geo:NaN,0.0").is_none());
        assert!(parse_geo_uri("geo:inf,0.0").is_none());

        // Not a geo URI at all. `https:` matters: it must not be mistaken
        // for one and then handed onward as a location.
        assert!(parse_geo_uri("https://example.org/1,2").is_none());
        assert!(parse_geo_uri("geo:").is_none());
        assert!(parse_geo_uri("geo:51.5008").is_none());
        assert!(parse_geo_uri("").is_none());
        assert!(parse_geo_uri("geo:abc,def").is_none());
    }

    #[test]
    fn building_a_uri_refuses_the_same_points_parsing_does() {
        // The two must agree, or Lightning can send something it would then
        // refuse to render.
        assert_eq!(geo_uri(51.5008, -0.1247).as_deref(),
                   Some("geo:51.500800,-0.124700"));
        assert!(geo_uri(91.0, 0.0).is_none());
        assert!(geo_uri(0.0, 181.0).is_none());
        assert!(geo_uri(f64::NAN, 0.0).is_none());
        assert!(geo_uri(f64::INFINITY, 0.0).is_none());

        // Round trip: everything this builds, the parser accepts.
        for (lat, lon) in [(0.0, 0.0), (-90.0, -180.0), (90.0, 180.0),
                           (51.5008, -0.1247)] {
            let uri = geo_uri(lat, lon).unwrap();
            let back = parse_geo_uri(&uri).unwrap();
            assert!((back.lat - lat).abs() < 1e-5, "{uri}");
            assert!((back.lon - lon).abs() < 1e-5, "{uri}");
        }
    }

    #[test]
    fn an_unparseable_uri_leaves_the_coordinates_absent() {
        // ABSENT, not zero. A zero pair is a point in the Atlantic, and a UI
        // reading it would draw a confident link to the wrong place. The
        // absence is what selects the plain-text rendering.
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
