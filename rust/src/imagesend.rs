//! Still images that carry a caller-rendered raster thumbnail, and the rule
//! for showing an SVG attachment.
//!
//! SVG is never decoded for display (CLAUDE.md §6). What a timeline row may
//! show for one is a raster thumbnail: on send, C++ renders one from the
//! user's own file and it crosses here as `PosterBytes`, re-validated by
//! magic sniffing; on receive, only the sender's thumbnail is fetched, and a
//! declared SVG without one is refused before any request, because the full
//! payload would be refused by MediaBridge's byte sniff and a homeserver
//! cannot thumbnail SVG (Synapse's thumbnailer is Pillow, which has no SVG
//! decoder).

use std::ffi::c_char;
use std::os::raw::c_void;

use matrix_sdk::attachment::{AttachmentInfo, Thumbnail};
use matrix_sdk_ui::timeline::AttachmentSource;

use crate::rooms::{attachment_info, PosterBytes};
use crate::RustClient;

/// True for a declared SVG type, plain or compressed, parameters ignored.
/// The declared type is sender-chosen, so this may only ever withhold media;
/// the byte sniff in MediaBridge is the enforcement.
pub(crate) fn is_svg_mimetype(mimetype: &str) -> bool {
    let essence = mimetype.split(';').next().unwrap_or("").trim();
    essence
        .get(..9)
        .is_some_and(|prefix| prefix.eq_ignore_ascii_case("image/svg"))
}

/// A thumbnail-class fetch (`kind` 1 or 2) for a declared SVG with no sender
/// thumbnail has nothing it could show: the full-payload fallback is markup
/// and a server-side thumbnail of SVG does not exist. Refused without a
/// request.
pub(crate) fn svg_thumbnail_unavailable(
    kind: u32,
    has_embedded_thumbnail: bool,
    mimetype: Option<&str>,
) -> bool {
    kind != 0 && !has_embedded_thumbnail && mimetype.is_some_and(is_svg_mimetype)
}

/// Event metadata and thumbnail for an outgoing still image. The thumbnail
/// is dropped (the image still sends) unless its bytes sniff as a raster
/// within `PosterBytes`' bounds, so SVG bytes labelled as a PNG never become
/// `thumbnail_info`.
pub(crate) fn image_send_parts(
    mime: &str,
    width: u64,
    height: u64,
    size: u64,
    thumbnail: Option<PosterBytes>,
) -> Result<(Option<AttachmentInfo>, Option<Thumbnail>), String> {
    if !mime.starts_with("image/") {
        return Err("image send requires an image type".to_owned());
    }
    let info = attachment_info(mime, width, height, size, 0);
    Ok((info, thumbnail.and_then(PosterBytes::into_thumbnail)))
}

fn checked_file_len(path: &str) -> Result<u64, String> {
    let metadata = std::fs::metadata(path)
        .map_err(|_| "attachment file is not readable".to_owned())?;
    if !metadata.is_file() {
        return Err("attachment path is not a regular file".to_owned());
    }
    if metadata.len() == 0 {
        return Err("attachment file is empty".to_owned());
    }
    Ok(metadata.len())
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn send_image_path(
    bridge: &RustClient,
    room_id: String,
    path: String,
    mime: String,
    caption: String,
    width: u64,
    height: u64,
    thumbnail: Option<PosterBytes>,
    op_id: u64,
) -> Result<(), String> {
    let size = checked_file_len(&path)?;
    let (info, thumbnail) = image_send_parts(&mime, width, height, size, thumbnail)?;
    let caption = if caption.trim().is_empty() { None } else { Some(caption) };
    bridge.timelines.send_attachment(
        &bridge.runtime,
        room_id,
        AttachmentSource::File(std::path::PathBuf::from(path)),
        mime,
        caption,
        info,
        thumbnail,
        op_id,
    )
}

/// Thread twin of `send_image_path`, via the thread-focused timeline. Never
/// falls back to a room send.
#[allow(clippy::too_many_arguments)]
pub(crate) fn send_thread_image_path(
    bridge: &RustClient,
    room_id: String,
    root_event_id: String,
    path: String,
    mime: String,
    caption: String,
    width: u64,
    height: u64,
    thumbnail: Option<PosterBytes>,
    op_id: u64,
) -> Result<(), String> {
    let size = checked_file_len(&path)?;
    let (info, thumbnail) = image_send_parts(&mime, width, height, size, thumbnail)?;
    let caption = if caption.trim().is_empty() { None } else { Some(caption) };
    let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
        return Err("Rust SDK session is not logged in.".to_owned());
    };
    bridge.timelines.send_thread_attachment(
        &bridge.runtime,
        client,
        room_id,
        root_event_id,
        AttachmentSource::File(std::path::PathBuf::from(path)),
        mime,
        caption,
        info,
        thumbnail,
        op_id,
    )
}

/// Send a still image with a caller-rendered raster thumbnail.
///
/// `thumb_*` is optional (null/0 sends none). The thumbnail is copied here,
/// bounded, and re-validated by magic sniffing; an invalid one is dropped and
/// the image still sends. The SDK uploads and encrypts it.
///
/// # Safety
/// String arguments must be valid NUL-terminated strings; `thumb_data` must
/// be null or point to at least `thumb_len` readable bytes.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_timeline_send_image(
    ptr: *mut c_void,
    room_id: *const c_char,
    local_path: *const c_char,
    mime: *const c_char,
    caption: *const c_char,
    width: u64,
    height: u64,
    thumb_data: *const u8,
    thumb_len: usize,
    thumb_width: u64,
    thumb_height: u64,
    op_id: u64,
) -> *mut c_char {
    crate::ffi_string(|| {
        let bridge = unsafe { crate::bridge(ptr)? };
        let room_id = unsafe { crate::cstr_arg(room_id) }?;
        let local_path = unsafe { crate::cstr_arg(local_path) }?;
        let mime = unsafe { crate::cstr_arg(mime) }?;
        let caption = unsafe { crate::cstr_arg(caption) }?;
        let thumbnail =
            unsafe { crate::poster_arg(thumb_data, thumb_len, thumb_width, thumb_height) };
        send_image_path(
            bridge, room_id, local_path, mime, caption, width, height,
            thumbnail, op_id,
        )
        .map(|_| String::new())
    })
}

/// Thread twin of `mx_rust_timeline_send_image`.
///
/// # Safety
/// As for `mx_rust_timeline_send_image`.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_thread_send_image(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
    local_path: *const c_char,
    mime: *const c_char,
    caption: *const c_char,
    width: u64,
    height: u64,
    thumb_data: *const u8,
    thumb_len: usize,
    thumb_width: u64,
    thumb_height: u64,
    op_id: u64,
) -> *mut c_char {
    crate::ffi_string(|| {
        let bridge = unsafe { crate::bridge(ptr)? };
        let room_id = unsafe { crate::cstr_arg(room_id) }?;
        let root = unsafe { crate::cstr_arg(root_event_id) }?;
        let local_path = unsafe { crate::cstr_arg(local_path) }?;
        let mime = unsafe { crate::cstr_arg(mime) }?;
        let caption = unsafe { crate::cstr_arg(caption) }?;
        let thumbnail =
            unsafe { crate::poster_arg(thumb_data, thumb_len, thumb_width, thumb_height) };
        send_thread_image_path(
            bridge, room_id, root, local_path, mime, caption, width, height,
            thumbnail, op_id,
        )
        .map(|_| String::new())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn png_bytes() -> Vec<u8> {
        let mut png = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
        png.extend_from_slice(&[0u8; 24]);
        png
    }

    fn thumb(data: Vec<u8>) -> Option<PosterBytes> {
        Some(PosterBytes { data, width: 400, height: 300 })
    }

    #[test]
    fn an_svg_image_keeps_its_raster_thumbnail() {
        let (info, thumbnail) =
            image_send_parts("image/svg+xml", 800, 600, 4096, thumb(png_bytes()))
                .expect("an image type is accepted");
        let thumbnail = thumbnail.expect("a sniffed PNG becomes the thumbnail");
        assert_eq!(thumbnail.content_type.essence_str(), "image/png");
        assert_eq!(u64::from(thumbnail.width), 400);
        assert_eq!(u64::from(thumbnail.height), 300);
        match info {
            Some(AttachmentInfo::Image(image)) => {
                assert_eq!(image.width.map(u64::from), Some(800));
                assert_eq!(image.height.map(u64::from), Some(600));
                assert_eq!(image.size.map(u64::from), Some(4096));
            }
            _ => panic!("an SVG is sent with image metadata"),
        }
    }

    #[test]
    fn svg_bytes_disguised_as_a_thumbnail_are_dropped_and_the_image_still_sends() {
        let disguised = [
            b"<svg xmlns=\"http://www.w3.org/2000/svg\"/>".to_vec(),
            b"<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>".to_vec(),
            [&[0x1Fu8, 0x8B][..], &[0u8; 30][..]].concat(),
        ];
        for data in disguised {
            let (info, thumbnail) =
                image_send_parts("image/svg+xml", 64, 64, 128, thumb(data))
                    .expect("the image itself still sends");
            assert!(thumbnail.is_none(), "non-raster thumbnail bytes are dropped");
            assert!(info.is_some());
        }
    }

    #[test]
    fn the_image_path_refuses_a_non_image_type() {
        assert!(image_send_parts("application/pdf", 1, 1, 1, None).is_err());
        assert!(image_send_parts("video/mp4", 1, 1, 1, thumb(png_bytes())).is_err());
    }

    #[test]
    fn svg_mimetypes_are_recognised_with_parameters_and_case() {
        assert!(is_svg_mimetype("image/svg+xml"));
        assert!(is_svg_mimetype("IMAGE/SVG+XML"));
        assert!(is_svg_mimetype(" image/svg+xml ; charset=utf-8"));
        assert!(is_svg_mimetype("image/svg+xml-compressed"));
        assert!(!is_svg_mimetype("image/png"));
        assert!(!is_svg_mimetype("image/sv"));
        assert!(!is_svg_mimetype(""));
        // A multi-byte character at the prefix boundary must not panic.
        assert!(!is_svg_mimetype("imagé/svg+xml"));
    }

    #[test]
    fn an_svg_without_a_sender_thumbnail_has_no_thumbnail_fallback() {
        let svg = Some("image/svg+xml");
        // Timeline thumbnail and list thumbnail: refused, no request.
        assert!(svg_thumbnail_unavailable(1, false, svg));
        assert!(svg_thumbnail_unavailable(2, false, svg));
        // With the sender's thumbnail, that thumbnail is fetched.
        assert!(!svg_thumbnail_unavailable(1, true, svg));
        assert!(!svg_thumbnail_unavailable(2, true, svg));
        // The full payload stays fetchable for Save As.
        assert!(!svg_thumbnail_unavailable(0, false, svg));
        // Rasters and undeclared types keep the existing fallback.
        assert!(!svg_thumbnail_unavailable(1, false, Some("image/png")));
        assert!(!svg_thumbnail_unavailable(1, false, None));
    }
}
