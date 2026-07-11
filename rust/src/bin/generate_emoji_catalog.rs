// Regenerates data/emoji-catalog.tsv from the pinned `emojis` crate.
// Run from the repository root with:
//   cargo run --offline --locked --manifest-path rust/Cargo.toml \
//     --bin generate_emoji_catalog > data/emoji-catalog.tsv

use emojis::{Emoji, Group, SkinTone};
use std::fmt::Write as _;

fn category(group: Group) -> &'static str {
    match group {
        Group::SmileysAndEmotion => "Smileys & Emotion",
        Group::PeopleAndBody => "People & Body",
        Group::AnimalsAndNature => "Animals & Nature",
        Group::FoodAndDrink => "Food & Drink",
        Group::TravelAndPlaces => "Travel & Places",
        Group::Activities => "Activities",
        Group::Objects => "Objects",
        Group::Symbols => "Symbols",
        Group::Flags => "Flags",
    }
}

fn tone_name(emoji: &Emoji) -> &'static str {
    match emoji.skin_tone() {
        Some(SkinTone::Default) => "default",
        Some(SkinTone::Light) => "light",
        Some(SkinTone::MediumLight) => "medium-light",
        Some(SkinTone::Medium) => "medium",
        Some(SkinTone::MediumDark) => "medium-dark",
        Some(SkinTone::Dark) => "dark",
        Some(_) => "mixed",
        None => "none",
    }
}

fn clean(value: &str) -> String {
    value.replace(['\t', '\n', '\r'], " ")
}

fn emit(output: &mut String, emoji: &Emoji, base: &str) {
    let aliases = emoji.shortcodes().collect::<Vec<_>>().join(" ");
    writeln!(
        output,
        "{}\t{}\t{}\t{}\t{}\t{}",
        emoji.as_str(),
        clean(emoji.name()),
        clean(&aliases),
        category(emoji.group()),
        base,
        tone_name(emoji),
    ).expect("writing to a String cannot fail");
}

fn main() {
    let path = std::env::args().nth(1).unwrap_or_else(|| {
        "data/emoji-catalog.tsv".to_owned()
    });
    let mut output = String::from(
        "# Lightning emoji catalogue; Unicode Emoji 17.0; emojis crate 0.8.2\n\
         # emoji\\tname\\taliases\\tcategory\\tbaseEmoji\\ttone\n",
    );
    for emoji in emojis::iter() {
        emit(&mut output, emoji, emoji.as_str());
        if let Some(tones) = emoji.skin_tones() {
            for variant in tones.skip(1) {
                emit(&mut output, variant, emoji.as_str());
            }
        }
    }
    std::fs::write(path, output).expect("failed to write emoji catalogue");
}
