//! MSC4193 spoiler media: reading the marker and sending marked media.
//!
//! The SDK's attachment path builds a typed `RoomMessageEventContent`, which
//! cannot carry the marker, so a spoiler send uploads through the SDK
//! (encrypted in an encrypted room) and queues the raw content itself.

use matrix_sdk::{
    room::reply::{EnforceThread, Reply},
    ruma::{
        events::{
            room::message::{
                AddMentions, ReplyWithinThread, RoomMessageEventContentWithoutRelation,
            },
            AnyMessageLikeEventContent,
        },
        serde::Raw,
        OwnedEventId,
    },
    Client, Room,
};
use serde_json::{json, Map, Value};

pub(crate) const UNSTABLE_SPOILER: &str = "page.codeberg.everypizza.msc4193.spoiler";
pub(crate) const UNSTABLE_SPOILER_REASON: &str =
    "page.codeberg.everypizza.msc4193.spoiler.reason";
pub(crate) const STABLE_SPOILER: &str = "m.spoiler";
const MAX_REASON_CHARS: usize = 200;

fn bounded_reason(reason: &str) -> String {
    reason
        .chars()
        .filter(|c| {
            !c.is_control()
                && !matches!(c, '\u{200e}'..='\u{200f}' | '\u{202a}'..='\u{202e}' | '\u{2066}'..='\u{2069}')
        })
        .take(MAX_REASON_CHARS)
        .collect::<String>()
        .trim()
        .to_owned()
}

/// `Some(reason)` (possibly empty) when `content` marks its media a spoiler.
pub(crate) fn spoiler_of(content: &Value) -> Option<String> {
    let object = content.as_object()?;
    let unstable = object.get(UNSTABLE_SPOILER).and_then(Value::as_bool) == Some(true);
    let stable = object
        .get(STABLE_SPOILER)
        .filter(|v| v.is_object() || v.as_bool() == Some(true));
    if !unstable && stable.is_none() {
        return None;
    }
    let reason = object
        .get(UNSTABLE_SPOILER_REASON)
        .and_then(Value::as_str)
        .or_else(|| stable.and_then(|s| s.get("reason")).and_then(Value::as_str))
        .unwrap_or("");
    Some(bounded_reason(reason))
}

/// The marker of a wire event, from its original content or its latest
/// edit's `m.new_content`: an edit from a client that drops the field must
/// not reveal the media.
pub(crate) fn event_spoiler(
    original: Option<&Value>,
    latest_edit: Option<&Value>,
) -> Option<String> {
    let from_original = original.and_then(|e| e.get("content")).and_then(spoiler_of);
    let from_edit = latest_edit
        .and_then(|e| e.get("content"))
        .and_then(|c| c.get("m.new_content"))
        .and_then(spoiler_of);
    match (from_original, from_edit) {
        (Some(a), Some(b)) => Some(if b.is_empty() { a } else { b }),
        (a, b) => a.or(b),
    }
}

fn mime_essence(mime: &str) -> String {
    mime.split(';').next().unwrap_or("").trim().to_ascii_lowercase()
}

pub(crate) fn spoiler_msgtype(mime: &str) -> Option<&'static str> {
    let essence = mime_essence(mime);
    if essence == "image/svg+xml" {
        return None;
    }
    if essence.starts_with("image/") {
        Some("m.image")
    } else if essence.starts_with("video/") {
        Some("m.video")
    } else if essence.starts_with("audio/") {
        Some("m.audio")
    } else {
        None
    }
}

/// Where uploaded bytes live: `url` (plain mxc) or `file` (EncryptedFile).
pub(crate) struct UploadedSource {
    pub key: &'static str,
    pub value: Value,
}

pub(crate) struct SpoilerThumb {
    pub source: UploadedSource,
    pub mimetype: String,
    pub size: u64,
    pub width: u64,
    pub height: u64,
}

pub(crate) struct SpoilerMediaSpec {
    pub filename: String,
    pub caption: Option<String>,
    pub mimetype: String,
    pub size: u64,
    pub width: u64,
    pub height: u64,
    pub duration_ms: u64,
    pub animated: bool,
}

pub(crate) fn spoiler_media_content(
    spec: &SpoilerMediaSpec,
    source: UploadedSource,
    thumbnail: Option<SpoilerThumb>,
) -> Result<Value, String> {
    let msgtype = spoiler_msgtype(&spec.mimetype)
        .ok_or_else(|| "Only images, videos and audio can be marked as spoilers.".to_owned())?;
    let mut info = Map::new();
    info.insert("mimetype".into(), spec.mimetype.clone().into());
    info.insert("size".into(), spec.size.into());
    if msgtype != "m.audio" {
        if spec.width > 0 && spec.height > 0 {
            info.insert("w".into(), spec.width.into());
            info.insert("h".into(), spec.height.into());
        }
        if msgtype == "m.image" {
            info.insert("is_animated".into(), spec.animated.into());
        }
        if let Some(thumb) = thumbnail {
            let thumb_key = if thumb.source.key == "file" {
                "thumbnail_file"
            } else {
                "thumbnail_url"
            };
            info.insert(thumb_key.into(), thumb.source.value);
            info.insert(
                "thumbnail_info".into(),
                json!({
                    "mimetype": thumb.mimetype,
                    "size": thumb.size,
                    "w": thumb.width,
                    "h": thumb.height,
                }),
            );
        }
    }
    if msgtype != "m.image" && spec.duration_ms > 0 {
        info.insert("duration".into(), spec.duration_ms.into());
    }
    let caption = spec.caption.as_deref().map(str::trim).filter(|c| !c.is_empty());
    let mut content = Map::new();
    content.insert("msgtype".into(), msgtype.into());
    content.insert(
        "body".into(),
        caption.unwrap_or(spec.filename.as_str()).to_owned().into(),
    );
    if caption.is_some() {
        content.insert("filename".into(), spec.filename.clone().into());
    }
    content.insert(source.key.into(), source.value);
    content.insert("info".into(), Value::Object(info));
    content.insert("m.mentions".into(), json!({}));
    content.insert(UNSTABLE_SPOILER.into(), true.into());
    Ok(Value::Object(content))
}

/// The SDK's reply/thread metadata for a send, as matrix-sdk-ui infers it.
pub(crate) fn spoiler_reply(
    thread_root: Option<OwnedEventId>,
    in_reply_to: Option<OwnedEventId>,
) -> Option<Reply> {
    let (event_id, enforce_thread) = match (thread_root, in_reply_to) {
        (Some(_), Some(target)) => {
            (target, EnforceThread::Threaded(ReplyWithinThread::Yes))
        }
        (Some(root), None) => (root, EnforceThread::Threaded(ReplyWithinThread::No)),
        (None, Some(target)) => (target, EnforceThread::MaybeThreaded),
        (None, None) => return None,
    };
    Some(Reply { event_id, enforce_thread, add_mentions: AddMentions::Yes })
}

/// Re-applies the marker after the typed reply round trip dropped it.
pub(crate) fn with_marker(mut content: Value) -> Value {
    if let Some(object) = content.as_object_mut() {
        object.insert(UNSTABLE_SPOILER.into(), true.into());
    }
    content
}

async fn upload(
    client: &Client,
    encrypted: bool,
    mime: &mime::Mime,
    data: Vec<u8>,
) -> Result<UploadedSource, String> {
    if encrypted {
        let mut cursor = std::io::Cursor::new(data);
        let file = client
            .upload_encrypted_file(&mut cursor)
            .await
            .map_err(|_| "The spoiler media could not be uploaded.".to_owned())?;
        let value = serde_json::to_value(&file)
            .map_err(|_| "The spoiler media could not be uploaded.".to_owned())?;
        Ok(UploadedSource { key: "file", value })
    } else {
        let response = client
            .media()
            .upload(mime, data, None)
            .await
            .map_err(|_| "The spoiler media could not be uploaded.".to_owned())?;
        Ok(UploadedSource { key: "url", value: response.content_uri.to_string().into() })
    }
}

pub(crate) struct SpoilerSend {
    /// Read on the runtime when set, so the GUI thread never reads the file.
    pub path: Option<std::path::PathBuf>,
    pub data: Vec<u8>,
    pub spec: SpoilerMediaSpec,
    pub poster: Option<crate::rooms::PosterBytes>,
    pub thread_root: Option<OwnedEventId>,
    pub in_reply_to: Option<OwnedEventId>,
}

/// How long uploading may take before the send is reported failed: two
/// minutes plus one second per 64 KiB, at most an hour.
pub(crate) fn upload_timeout(bytes: u64) -> std::time::Duration {
    std::time::Duration::from_secs((120 + bytes / (64 * 1024)).min(3600))
}

/// Reads, uploads and builds the event. Runs outside the room's ordered send
/// lane so a long upload never holds back text sends.
pub(crate) async fn prepare_spoiler(
    client: &Client,
    room: &Room,
    request: SpoilerSend,
) -> Result<Value, String> {
    let SpoilerSend { path, data, mut spec, poster, thread_root, in_reply_to } = request;
    let data = match path {
        Some(path) => tokio::fs::read(&path)
            .await
            .map_err(|_| "attachment file is not readable".to_owned())?,
        None => data,
    };
    if data.is_empty() {
        return Err("attachment data is empty".to_owned());
    }
    if mime_essence(&spec.mimetype).starts_with("image/")
        && crate::rooms::sniff_image_mime(&data).is_none()
    {
        return Err("The image is not a supported raster format.".to_owned());
    }
    spec.size = data.len() as u64;
    let mime: mime::Mime = spec
        .mimetype
        .parse()
        .map_err(|_| "Unsupported attachment MIME type.".to_owned())?;
    spoiler_msgtype(&spec.mimetype)
        .ok_or_else(|| "Only images, videos and audio can be marked as spoilers.".to_owned())?;
    let encrypted = room
        .latest_encryption_state()
        .await
        .map_err(|_| "The room's encryption state is unknown.".to_owned())?
        .is_encrypted();
    let source = upload(client, encrypted, &mime, data).await?;
    let thumbnail = match poster.and_then(crate::rooms::PosterBytes::into_thumbnail) {
        Some(thumb) => {
            let mimetype = thumb.content_type.to_string();
            let size = u64::from(thumb.size);
            let (width, height) = (u64::from(thumb.width), u64::from(thumb.height));
            let source = upload(client, encrypted, &thumb.content_type, thumb.data).await?;
            Some(SpoilerThumb { source, mimetype, size, width, height })
        }
        None => None,
    };
    let content = spoiler_media_content(&spec, source, thumbnail)?;
    match spoiler_reply(thread_root, in_reply_to) {
        Some(reply) => {
            let typed: RoomMessageEventContentWithoutRelation =
                serde_json::from_value(content)
                    .map_err(|_| "The spoiler event could not be built.".to_owned())?;
            let replied = room
                .make_reply_event(typed, reply)
                .await
                .map_err(|_| "The replied-to message could not be loaded.".to_owned())?;
            Ok(with_marker(
                serde_json::to_value(&replied)
                    .map_err(|_| "The spoiler event could not be built.".to_owned())?,
            ))
        }
        None => Ok(content),
    }
}

pub(crate) async fn queue_spoiler(room: &Room, content: Value) -> Result<(), String> {
    let raw = serde_json::value::to_raw_value(&content)
        .map_err(|_| "The spoiler event could not be built.".to_owned())?;
    let queue = room.send_queue();
    queue.set_enabled(true);
    queue
        .send_raw(Raw::<AnyMessageLikeEventContent>::from_json(raw), "m.room.message".to_owned())
        .await
        .map_err(|_| "The spoiler event could not be queued.".to_owned())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec(mime: &str, caption: Option<&str>) -> SpoilerMediaSpec {
        SpoilerMediaSpec {
            filename: "cat.png".to_owned(),
            caption: caption.map(str::to_owned),
            mimetype: mime.to_owned(),
            size: 1234,
            width: 640,
            height: 480,
            duration_ms: 5000,
            animated: false,
        }
    }

    #[test]
    fn unstable_boolean_marks_a_spoiler_and_false_does_not() {
        assert_eq!(spoiler_of(&json!({ UNSTABLE_SPOILER: true })), Some(String::new()));
        assert_eq!(spoiler_of(&json!({ UNSTABLE_SPOILER: false })), None);
        assert_eq!(spoiler_of(&json!({ UNSTABLE_SPOILER: "true" })), None);
        assert_eq!(spoiler_of(&json!({ "msgtype": "m.image" })), None);
    }

    #[test]
    fn reason_comes_from_the_unstable_key_or_the_stable_object() {
        assert_eq!(
            spoiler_of(&json!({ UNSTABLE_SPOILER: true, UNSTABLE_SPOILER_REASON: "ending" })),
            Some("ending".to_owned())
        );
        assert_eq!(
            spoiler_of(&json!({ STABLE_SPOILER: { "reason": "plot" } })),
            Some("plot".to_owned())
        );
        assert_eq!(spoiler_of(&json!({ STABLE_SPOILER: {} })), Some(String::new()));
        assert_eq!(spoiler_of(&json!({ STABLE_SPOILER: "x" })), None);
    }

    #[test]
    fn reason_is_bounded_and_stripped_of_control_characters() {
        let long = "a".repeat(1000);
        let out = spoiler_of(&json!({ UNSTABLE_SPOILER: true, UNSTABLE_SPOILER_REASON: long }))
            .unwrap();
        assert_eq!(out.chars().count(), MAX_REASON_CHARS);
        let out = spoiler_of(
            &json!({ UNSTABLE_SPOILER: true, UNSTABLE_SPOILER_REASON: "a\u{202e}\nb" }),
        )
        .unwrap();
        assert_eq!(out, "ab");
    }

    #[test]
    fn an_edit_without_the_marker_keeps_the_media_hidden() {
        let original = json!({ "content": { "msgtype": "m.image", UNSTABLE_SPOILER: true } });
        let edit = json!({ "content": { "m.new_content": { "msgtype": "m.image" } } });
        assert_eq!(event_spoiler(Some(&original), Some(&edit)), Some(String::new()));
        let edit_marks = json!({ "content": { "m.new_content": { STABLE_SPOILER: {} } } });
        assert_eq!(
            event_spoiler(Some(&json!({ "content": {} })), Some(&edit_marks)),
            Some(String::new())
        );
        assert_eq!(event_spoiler(None, None), None);
    }

    #[test]
    fn content_carries_the_marker_cinny_reads() {
        let content = spoiler_media_content(
            &spec("image/png", None),
            UploadedSource { key: "url", value: "mxc://hs/abc".into() },
            None,
        )
        .unwrap();
        assert_eq!(content["msgtype"], "m.image");
        assert_eq!(content[UNSTABLE_SPOILER], true);
        assert_eq!(content["url"], "mxc://hs/abc");
        assert_eq!(content["body"], "cat.png");
        assert!(content.get("filename").is_none());
        assert_eq!(content["info"]["w"], 640);
        assert_eq!(content["info"]["is_animated"], false);
        assert!(content.get(STABLE_SPOILER).is_none());
        assert_eq!(spoiler_of(&content), Some(String::new()));
    }

    #[test]
    fn encrypted_video_with_caption_and_poster() {
        let file = json!({ "url": "mxc://hs/enc", "v": "v2" });
        let thumb = SpoilerThumb {
            source: UploadedSource { key: "file", value: json!({ "url": "mxc://hs/t" }) },
            mimetype: "image/jpeg".to_owned(),
            size: 10,
            width: 32,
            height: 18,
        };
        let content = spoiler_media_content(
            &spec("video/mp4", Some("  watch  ")),
            UploadedSource { key: "file", value: file.clone() },
            Some(thumb),
        )
        .unwrap();
        assert_eq!(content["msgtype"], "m.video");
        assert_eq!(content["file"], file);
        assert!(content.get("url").is_none());
        assert_eq!(content["body"], "watch");
        assert_eq!(content["filename"], "cat.png");
        assert_eq!(content["info"]["duration"], 5000);
        assert_eq!(content["info"]["thumbnail_file"]["url"], "mxc://hs/t");
        assert!(content["info"].get("thumbnail_url").is_none());
        assert_eq!(content["info"]["thumbnail_info"]["w"], 32);
    }

    #[test]
    fn typed_round_trip_drops_the_marker_and_with_marker_restores_it() {
        let content = spoiler_media_content(
            &spec("image/jpeg", None),
            UploadedSource { key: "url", value: "mxc://hs/abc".into() },
            None,
        )
        .unwrap();
        let typed: RoomMessageEventContentWithoutRelation =
            serde_json::from_value(content).unwrap();
        let back = serde_json::to_value(&typed).unwrap();
        assert!(back.get(UNSTABLE_SPOILER).is_none());
        assert_eq!(with_marker(back)[UNSTABLE_SPOILER], true);
    }

    #[test]
    fn an_animated_image_says_so_and_a_video_does_not_carry_the_hint() {
        let mut animated = spec("image/GIF", None);
        animated.animated = true;
        let content = spoiler_media_content(
            &animated,
            UploadedSource { key: "url", value: "mxc://hs/a".into() },
            None,
        )
        .unwrap();
        assert_eq!(content["info"]["is_animated"], true);
        let video = spoiler_media_content(
            &spec("video/mp4", None),
            UploadedSource { key: "url", value: "mxc://hs/v".into() },
            None,
        )
        .unwrap();
        assert!(video["info"].get("is_animated").is_none());
    }

    #[test]
    fn upload_timeout_grows_with_size_and_is_capped() {
        assert_eq!(upload_timeout(0).as_secs(), 120);
        assert_eq!(upload_timeout(64 * 1024 * 60).as_secs(), 180);
        assert_eq!(upload_timeout(u64::MAX).as_secs(), 3600);
    }

    #[test]
    fn mime_parameters_and_case_do_not_hide_an_image() {
        assert_eq!(mime_essence(" Image/PNG; charset=x"), "image/png");
    }

    #[test]
    fn only_media_types_can_be_spoilers() {
        assert_eq!(spoiler_msgtype("image/png"), Some("m.image"));
        assert_eq!(spoiler_msgtype("video/webm; codecs=vp9"), Some("m.video"));
        assert_eq!(spoiler_msgtype("audio/ogg"), Some("m.audio"));
        assert_eq!(spoiler_msgtype("image/svg+xml"), None);
        assert_eq!(spoiler_msgtype("application/pdf"), None);
        assert!(spoiler_media_content(
            &spec("application/pdf", None),
            UploadedSource { key: "url", value: "mxc://hs/a".into() },
            None
        )
        .is_err());
    }

    #[test]
    fn reply_shapes_follow_the_timeline_rules() {
        let root = OwnedEventId::try_from("$root:hs").unwrap();
        let target = OwnedEventId::try_from("$target:hs").unwrap();
        assert!(spoiler_reply(None, None).is_none());
        let r = spoiler_reply(None, Some(target.clone())).unwrap();
        assert!(matches!(r.enforce_thread, EnforceThread::MaybeThreaded));
        assert_eq!(r.event_id, target);
        let r = spoiler_reply(Some(root.clone()), None).unwrap();
        assert!(matches!(r.enforce_thread, EnforceThread::Threaded(ReplyWithinThread::No)));
        assert_eq!(r.event_id, root);
        let r = spoiler_reply(Some(root), Some(target.clone())).unwrap();
        assert!(matches!(r.enforce_thread, EnforceThread::Threaded(ReplyWithinThread::Yes)));
        assert_eq!(r.event_id, target);
    }
}
