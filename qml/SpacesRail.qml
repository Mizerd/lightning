import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7 design shell: the far-left rail (68 px). Top-to-bottom: Home ("all
// rooms"), Space avatars (40×40, radius 12, active = accent outline), then a
// bottom cluster with Settings and the account avatar that opens the account
// switcher popover. The rail is always visible — it is the primary
// navigation column, not a Spaces-only affordance.
//
// ── Dragging ──────────────────────────────────────────────────────────────
//
// The rows come from `app.railEntries` (a real QAbstractListModel), NOT from a
// JavaScript array. That is what makes the drag feel like Element's: the model
// emits a genuine `beginMoveRows` for the preview position, so the neighbours
// ANIMATE out of the way while the pointer is still down, and the delegate
// holding the gesture is never destroyed by a refresh underneath it. The
// previous version rebuilt a JS array on every change, which is a model reset:
// no move transition, every delegate torn down, and the resulting position
// only discoverable after release — reported as "kinda hard to tell exactly
// where you are moving them".
//
// What a drag looks like: THE TILE ITSELF MOVES. It follows the pointer at
// full opacity and its neighbours animate out of the way around it, which is
// Element's behaviour and what was asked for in those words — "spaces should
// always be their normal image and move freely without a line appearing
// between them".
//
// An earlier revision drew three things at once (a dimmed gap where the tile
// would land, an accent insertion line at that gap, and a floating copy of the
// tile under the pointer). All three are gone: the tile IS the feedback, and
// where it currently sits IS where it will land, so a separate line claiming
// the same thing was noise on 68 px of chrome.
//
// The one thing still drawn on top of the movement is the GROUP target: a ring
// on the Space or folder a release would file into. It arms the moment the
// pointer is on that tile and needs no dwell to be safe, because NOTHING MOVES
// while the pointer is on a tile — dragging THROUGH one on the way somewhere
// else changes the order not at all. See `readingAt` below for why that rule
// replaced two earlier ones that made grouping unreachable.
Rectangle {
    id: root
    color: AppTheme.rail

    // Emitted by the Add Space tile below the Space list; MainScreen routes
    // it into the creation dialog's Space mode.
    signal createSpaceRequested()

    // The Direct Messages tab, CHANNELS ONLY. Classic reaches DMs through its
    // People filter chip over one activity-ordered list; a tab there would be
    // a second route to the same rows in a layout the maintainer asked to
    // leave untouched.
    readonly property bool peopleTabVisible:
        app.settings && app.settings.roomNavigationLayout === 1
    Binding {
        target: app.railEntries
        property: "peopleEntryVisible"
        value: root.peopleTabVisible
    }
    // "Other rooms" is the mirror image: CLASSIC only. The tile narrows a Home
    // that shows everything, which is what Classic's Home is; Channels' Home
    // already lists exactly the rooms in no Space, so the two tiles opened the
    // same page. Reported live 2026-09-03.
    Binding {
        target: app.railEntries
        property: "orphansEntryVisible"
        value: !root.peopleTabVisible
    }
    // The same condition, told to the model that computes the badges. A
    // tile's badge counts what that tile's view lists, and Home lists
    // different things in the two layouts: in Channels the DMs are on the
    // tile above and every Space's rooms are on its own tile, so Home
    // counting them pointed at a message that could not be there. That is how
    // two direct messages were missed on 2026-08-31.
    Binding {
        target: app.spaces
        property: "directMessagesHaveOwnTile"
        value: root.peopleTabVisible
    }
    // A SELECTION THAT NO LONGER HAS A TILE MUST NOT SURVIVE. Switching to
    // Classic removes the People tab, and leaving the selection on it would
    // leave the whole shell scoped to a tab with nothing rendering it — the
    // room list scoped to DMs with no visible control saying so, and no way
    // back but to click a Space.
    onPeopleTabVisibleChanged: {
        if (!peopleTabVisible && app.spaces
            && app.spaces.activeSpaceId === "@people")
            app.spaces.activeSpaceId = ""
        // And the same rescue the other way: switching to Channels removes
        // the "Other rooms" tile, and a selection left on it would scope the
        // shell to a tile with nothing rendering it. Home is where it lands,
        // which in Channels shows the very same rooms.
        if (peopleTabVisible && app.spaces
            && app.spaces.activeSpaceId === "@orphans")
            app.spaces.activeSpaceId = ""
    }

    // How many of a Space's rooms are revealed under its tile. SESSION state,
    // unlike the expansion itself: "show me five more" is a momentary
    // request, where "this Space is open" is how the user wants to navigate
    // and is persisted by RailLayoutStore. Held on the rail root so ListView
    // recycling and model resets never forget it.
    property var railReveal: ({})
    // Bumped on every SpaceManager change so the revealed-rooms bindings
    // (function calls, which QML tracks through this read) re-evaluate.
    property int spacesRevision: 0

    /// A row's height — its tile plus air above and below. The divider row
    /// (Home, or People when it is shown) adds its own band for the handoff
    /// rule beneath it.
    ///
    /// ONE SOURCE, because the drag's row arithmetic accumulates these and
    /// the delegate draws them: a literal in either place is a mis-drop at
    /// any interface size but the one it was written at.
    readonly property int normalRowBand: railTileSize + 2 * rowPad
    readonly property int dividerRowBand: normalRowBand + AppTheme.scaled(10)
    readonly property int railRoomRowBand: railRoomTileSize + 2 * rowPad

    /// ── THE TILES DO NOT MOVE, AND NOTHING IS DRAWN BETWEEN THEM ───────
    ///
    /// Indenting per level was the first answer and it was reported as "they
    /// keep sticking out more and more and create like a wave pattern". Moving
    /// the depth into connector lanes was the second, and a visual audit
    /// measured what that cost: 59% of a 116px rail spent on line-work and
    /// void, with depth encoded as the LENGTH of a horizontal rule — 10px per
    /// level at the widest stop and FOUR at the default width — and lanes that
    /// began in empty background, never attaching to the parent they stood
    /// for. It read as a wiring diagram with avatars stapled to one edge.
    ///
    /// So there are no lines. Containment is a tinted REGION behind the run,
    /// which is what Discord does, what this rail's own folders already did,
    /// and the one device that costs no horizontal space at all. Depth beyond
    /// the first step is carried by SIZE — a Space, a subspace, a room — which
    /// is Element's answer and is also free.
    ///
    /// THE GUTTER IS SIZED BY WHAT IT HOLDS, and it was not: this margin was
    /// a literal 14 with nothing tying it to the expander it has to carry.
    /// At the narrowest rail that left the chevron 2.8px from the rail's own
    /// outer edge and 4px from its tile — hard against the window frame, and
    /// reading as something that had fallen off the column rather than as
    /// something belonging to the tile beside it.
    ///
    /// IT WAS NOT CLIPPED, and the first version of this comment said it was.
    /// That came from eyeballing a 2x upscale and assuming the glyph was as
    /// wide as the size it is given; an `Icon` sizes by FONT PIXEL SIZE and a
    /// chevron's advance is 7.2px of the 12 it was asked for, so the arithmetic
    /// that predicted x = -2 was out by the difference. Measure the item.
    ///
    /// ── AND THE GUTTER WAS BUDGETED AGAINST A PHANTOM ────────────────
    ///
    /// It was sized from the chevron's ADVANCE (7.2px of the 12 it is given).
    /// Its INK is 3.3. The glyph was right-anchored, so a quarter of the
    /// gutter was the font's own empty side bearing — and the visible mark
    /// ended up with 13px of bare rail on one side and 9px on the other,
    /// beside a 59px tile. Measured: the proximity gap was 2.7x the mark's
    /// own width, so it read as debris in the frame rather than as a control
    /// ON the tile. (For scale: the search magnifier one column right has
    /// 12x12 of ink. This had 3.3x6.0 — a seventh of the area.)
    ///
    /// So it is positioned by its INK, not by its box, and the box's centre
    /// and the ink's centre coincide to a quarter-pixel (measured, not
    /// assumed). Bigger too: 16 where it was 12.
    ///
    /// Moving the chevron per level was considered and refused: it is what
    /// "the chevrons are unevenly distanced" already asked to have removed.
    readonly property int chevronGlyphSize: AppTheme.scaled(16)
    // `chevronInset` lived here and had no reader anywhere in the tree once
    // the plate took over the gutter's budget — while a comment still cited
    // it as one of the gutter's four tenants. A token nothing reads is a
    // number that cannot be wrong, which is exactly how it outlived the
    // thing it described.
    /// The ink inside that box — about 0.26 of the font size for this glyph.
    /// Used to place it, because placing the BOX leaves a quarter of the
    /// gutter as side bearing and pushes the mark away from its tile.
    /// 0.36, and it was 0.26 — 4px of ink for a control, which a critique
    /// could not find on screen without knowing where to look. The gutter is
    /// 14 and the advance about 9, so this still clears both sides.
    readonly property int chevronInkWidth: Math.round(chevronGlyphSize * 0.36)
    /// Ink to tile. Two pixels: a control belongs to the thing it acts on.
    readonly property int chevronTileGap: AppTheme.scaled(2)
    /// HOW FAR THE ACTIVE RING REACHES OUTSIDE ITS TILE, and the chevron's
    /// gap is measured from THAT, not from the tile.
    ///
    /// Reported as "clipping" on 2026-09-18, with an arrow at a selected
    /// Space whose expander had lost its right arm. Nothing was clipped: the
    /// 2px accent ring is drawn OUTSIDE the tile's bounds, so a selected
    /// tile's visible edge is not `tileColumnX`, and a gap measured to the
    /// tile put the glyph's ink exactly where the ring paints. The two
    /// overlapped, and only ever on the one tile the user had just clicked,
    /// which is why every capture taken while auditing this column looked
    /// fine. (WHICH ONE PAINTED OVER WHICH is not what made it unreadable
    /// and an earlier version of this comment asserted it backwards: the
    /// chevron is declared AFTER the ring and neither carries a `z`, so the
    /// glyph drew on top of a saturated accent stroke. A thin
    /// `textSecondary` arm on the accent reads as missing either way. §16
    /// already carries this lesson for this exact glyph — measure it, do not
    /// narrate it.)
    ///
    /// The ring came in from 4 to 2 as part of that, which is also the
    /// tighter reading: at 4 it floated, unattached to the tile it marks.
    /// THE GUTTER'S BUDGET IS WRITTEN IN ONE PLACE ONLY, at
    /// `railSideMargin` — it was briefly written in three, two of them
    /// already stale by the time they were read.
    readonly property int tileRingOutset: AppTheme.scaled(2)
    /// THE LEFT WALL OF THE CHEVRON'S SLOT: the deepest region inset any row
    /// can carry. Pulling the glyph in to clear the ring pushed it OUT of the
    /// innermost region at the minimum width — the same "a shape left its
    /// region" defect this column was audited for a day earlier, reintroduced
    /// by the fix for the ring. Derived from the cap rather than from the
    /// row's own depth on purpose: a chevron that moves per level is what
    /// "the chevrons are unevenly distanced" already asked to have removed.
    ///
    /// The gutter is exactly full at the minimum width; its arithmetic is
    /// at `railSideMargin`, which is the one place that states it.
    readonly property int chevronSlotLeft: bandInset(maxBandLayers)
    /// THE SELF BADGE SITS ON THE AVATAR'S EDGE AND COVERS NONE OF THE FACE.
    ///
    /// It was anchored to the tile's square bounding box at a 6.5% margin,
    /// which put its centre 15.4px from the centre of a disc of radius 20 —
    /// INSIDE the avatar, so it sat on the picture. Reported as covering the
    /// profile.
    ///
    /// Placed by the geometry instead: the centre goes
    /// `avatarR + dotR - ring` from the avatar's centre along the 45-degree
    /// diagonal, so the state ink lands exactly at the disc's edge and only
    /// the ring — which is rail-coloured, and whose whole job is to separate
    /// the badge from what is under it — overlaps the picture at all.
    readonly property int selfDotSize: AppTheme.scaled(13)
    /// PresenceDot insets its state disc by 2px; the fallback below matches
    /// it so both land in the same place.
    readonly property int selfDotRing: 2
    readonly property real selfDotMargin:
        railTileSize
        - (railTileSize / 2
           + (railTileSize / 2 + selfDotSize / 2 - selfDotRing) / Math.SQRT2
           + selfDotSize / 2)
    /// THE PLATE THE MARK SITS IN. A chevron alone in a column reads as
    /// debris — it was described exactly that way twice, once as "stray
    /// marks in the gutter" and once as "out of place alone" — so it gets a
    /// surface, resting as well as hovered, and the hover state is now a
    /// brightening of something already there rather than a shape appearing
    /// out of nothing.
    /// SQUARE, AND SIZED FOR THE WIDER OF THE TWO GLYPHS.
    ///
    /// The first plate was 10 wide by 22 tall and the maintainer reported it
    /// as changing size between states. It never changed size — the INK did.
    /// Measured off a capture at dpr 1.5, `expand_more`'s ink is 12x6 device
    /// px and `chevron_right`'s is 7x12: the two are transposes of each
    /// other, so a tall narrow pill holds a wide flat mark in one state and a
    /// tall thin one in the other, and the pair reads as two different boxes.
    /// A SQUARE plate is the only shape that looks the same around both.
    /// Sized from the wider ink (8 logical) plus a real 2px of air.
    readonly property int chevronPlatePad: AppTheme.scaled(2)
    readonly property int chevronInkLongest: AppTheme.scaled(8)
    readonly property int chevronPlateWidth:
        chevronInkLongest + 2 * chevronPlatePad
    readonly property int chevronPlateHeight: chevronPlateWidth
    /// AND THE MARK IS NOT CENTRED IN ITS OWN BOX. Same capture: with the
    /// plate centred on the glyph's box, `expand_more`'s ink sat 3 device px
    /// nearer the top than the bottom (12 above, 15 below) — 1 logical px
    /// high — while `chevron_right`'s was within half a pixel. So the glyph
    /// is nudged DOWN by that much when it is the one that needs it, which
    /// is the vertical half of the same rule the x already follows: place the
    /// INK, never the box.
    readonly property int chevronInkRise: AppTheme.scaled(1)
    /// ONE place decides where the control goes, and the glyph and its plate
    /// both read it. The PLATE is what has to clear the ring now — it is the
    /// control's real edge — so the gap is measured from the plate and the
    /// ink follows it inwards.
    readonly property real chevronPlateLeft:
        Math.max(chevronSlotLeft,
                 tileColumnX - tileRingOutset - chevronTileGap
                 - chevronPlateWidth)
    readonly property real chevronInkLeft:
        chevronPlateLeft + (chevronPlateWidth - chevronInkWidth) / 2
    /// NOTE: the box's width is the glyph's ADVANCE, not `chevronGlyphSize`,
    /// and the ink centres in the ADVANCE (measured: box centre and ink
    /// centre agree to a quarter-pixel). So the placement below is written at
    /// the Icon itself, where its own `width` is in scope — computing it here
    /// from the font SIZE put the mark 5px from its tile instead of 2.
    /// ── THE TWO MARGINS ARE NOT THE SAME, AND THEY WERE ───────────────
    ///
    /// One `railSideMargin` was mirrored on both edges, so the RIGHT side of
    /// the rail was as wide as the chevron column on the left while holding
    /// nothing at all — reported as "the space bar is a bit too wide for
    /// comfort, especially on the right side, it's just empty space there",
    /// and it was: 24px of it at every width.
    ///
    /// The left gutter is a COLUMN with a control in it and is sized by that
    /// control. The right margin is air, so it is sized like air. The rail
    /// loses 16px at every stop and the tiles keep every pixel of theirs.
    /// ONE MARGIN, BOTH SIDES. The asymmetric pair that replaced it was the
    /// right fix for the wrong problem: it did remove dead space on the right
    /// and it moved the tile column 8.8px off the rail's own centre, which a
    /// design audit measured as a 3.3:1 split and the maintainer reported as
    /// "top and bottom ui is not centered and stuck to the right side".
    ///
    /// THE RAIL HELD THREE DISAGREEING CENTRE LINES AT ONCE, all visible in
    /// its bottom 200px: the tile column, the Home divider, and the bottom
    /// separator — and the separator was the one that was CORRECTLY centred
    /// on the rail, which is exactly why the cog and avatar beside it looked
    /// wrong. The separator published the true centre and the tiles refused
    /// it.
    ///
    /// 14 beside a 44px tile in a 72px rail is Discord's proportion. The
    /// earlier complaint that the margins were "just empty space there" was
    /// about 24px of void beside a 40px tile in an 88px one.
    /// 14 until 2026-09-18, when the expander was given a PLATE to sit in
    /// ("they seem out of place alone") and the gutter had to hold one more
    /// tenant. It is a column with a control in it and it is now sized by the
    /// whole control rather than by the mark inside it:
    /// inset 3 + plate 12 + gap 2 + ring 2 = 19.
    ///
    /// AND IT COSTS WIDTH ON BOTH SIDES, deliberately. This is ONE token for
    /// both edges because the tile column is centred on it, and centring is
    /// what fixed "top and bottom ui is not centered and stuck to the right
    /// side". So sizing the left gutter by its tenants adds the same air on
    /// the right, and the rail goes 68 -> 78 logical px at 100%. That is the
    /// price of the gutter holding a real control rather than a loose mark,
    /// and it is worth saying out loud because this file also records "the
    /// space bar is a bit too wide for comfort" as a report from the same
    /// maintainer. If it has to come back, the plate is the thing to shrink.
    readonly property int railSideMargin: AppTheme.scaled(19)
    /// THE WIDTH NOW BUYS SOMETHING. It used to buy indent, then lanes; both
    /// were spent on structure rather than on content, so dragging the rail
    /// wider changed the tiles by nothing at all. Past the default the tile
    /// itself grows, up to a ceiling, and then the margins take the rest.
    /// ── 40..48, AND IT WAS 40..56 ─────────────────────────────────────
    ///
    /// "Icons are way too big", and the measurement that settles it is the
    /// product's own ladder in one screenshot: a room-list avatar is 21, a
    /// "Jump back in" avatar 33, the WELCOME HERO PORTRAIT 57 — and the rail
    /// tile was 59. A persistent navigation chip was larger than the hero.
    ///
    /// 44 rather than Discord's 48, because Discord's rail carries top-level
    /// servers only and this one also carries four tint layers, a nested tile
    /// step and revealed room tiles. A denser column needs a smaller unit.
    /// The derived steps stay legible: x0.85 -> 37, x0.7 -> 31, and at the
    /// 40 floor a room tile is 28, which is the bottom for two initials. At
    /// 59 the REVEALED ROOM tile was 41 — larger than a Discord server icon,
    /// for a room.
    readonly property int railTileSize:
        Math.min(AppTheme.scaled(48),
                 Math.max(AppTheme.scaled(40),
                          width - 2 * railSideMargin))
    /// PROPORTIONAL, and it was a raw `radiusLg` of 12 against a scaled tile
    /// — so the corner ratio moved with the UI font. At 59 it was 0.203,
    /// which is boxy; 0.27 is the squircle band (iOS 0.225, Discord 0.33).
    /// 0.30 of the tile, and PROPORTIONAL AT EVERY TIER through
    /// `tileRadiusFor()` below. A flat radius made a 34px room tile 40%
    /// squarer than the 41px subspace above it, which cancels part of the
    /// size ladder those two tiers exist to express.
    readonly property int railTileRadius: tileRadiusFor(railTileSize)
    function tileRadiusFor(size) { return Math.round(size * 0.30) }
    /// One rule for the glyph inside a pseudo tile — Home, People, the
    /// settings cog, the "+". They were a RAW 22 (unscaled, a real defect at
    /// 140%), a scaled 22 and a scaled 20, all in one 59px box: a ratio of
    /// 0.37 against Material's and Discord's 0.50.
    readonly property int railChipIconSize: Math.round(railTileSize * 0.5)
    /// Both dividers, one rule. The Home handoff divider was `x:
    /// tileColumnX` with 80% of the tile's width — flush with the tile's LEFT
    /// edge and 20% short on the right, centred under nothing.
    readonly property int railDividerWidth: Math.round(railTileSize * 0.8)
    readonly property int railDividerX:
        tileColumnX + Math.round((railTileSize - railDividerWidth) / 2)
    /// ONE x for every tile, CENTRED — the audit's one unambiguous keep, and
    /// centring is what R1 above restores. Past the tile's clamp the extra
    /// width splits evenly, so dragging the rail wider buys symmetric air
    /// rather than one fat side.
    readonly property int tileColumnX: Math.round((width - railTileSize) / 2)
    /// ONE step down for anything nested, however deep — the same decision
    /// Element makes with its 32 -> 24 avatar, and for the same reason: a
    /// per-level shrink runs out after three steps.
    /// 0.833, and it was 0.85 — which at a 48px tile gives 41, an ODD width
    /// on an even rail, so the nested rung centred on x=38.5 where every
    /// other tile in the column centres on 38.0. Half a pixel, on the one
    /// rule this rail has: every tile shares one axis. 0.833 gives 40.
    readonly property int railNestedTileSize: Math.round(railTileSize * 0.833)
    /// A REVEALED ROOM'S tile. 0.7 of the Space tile, which is what the
    /// literal 28 was at the size it was written at.
    readonly property int railRoomTileSize: Math.round(railTileSize * 0.7)
    /// Air above and below a tile inside a run, and the extra a run adds
    /// after its last row so the next group reads as a separate thing.
    readonly property int rowPad: AppTheme.scaled(4)
    /// The gap between two tiles in the column. ONE number, so the bottom
    /// cluster breathes like the list above it rather than at `spacing8`.
    readonly property int railTileGap: 2 * rowPad + AppTheme.spacing4
    /// TWO SIZES, BECAUSE THERE ARE TWO MEANINGS. One gap ran every break in
    /// the rail, and measured against the rows' own heights that made
    /// "leaving a nested region" and "an entirely different top-level Space"
    /// land 2.3px apart — the largest semantic break in the column did not
    /// read as one. A nested close needs only enough air to show the
    /// parent's tint through it; a top-level break has nothing behind it and
    /// has to carry on its own.
    readonly property int groupGap: AppTheme.scaled(8)
    readonly property int groupBreakGap: AppTheme.scaled(18)
    /// ── THE REGIONS STACK, one per ANCESTOR ───────────────────────────
    ///
    /// The first version drew ONE region per row, tinted by that row's own
    /// depth, and a depth-2 run therefore REPLACED its parent's tint for the
    /// rows it covered: three runs under one Space read as three unrelated
    /// bands stacked vertically rather than as two things inside a third.
    ///
    /// Every row now draws a region for each of its ancestors, outermost
    /// first, each one inset and a step further from the rail than the one
    /// containing it. A parent's region therefore runs unbroken behind every
    /// descendant it owns, and the nesting is visible as LAYERS.
    ///
    /// CAPPED AT FOUR, and it was three. Asked directly — "are these
    /// supposed to be the same color?" — of two regions that are nested one
    /// inside the other and wear the same tint, because both sit at or past
    /// the cap. They are, and that is the cap admitting it cannot say
    /// "deeper" any more; the honest fix is to make the cap deeper rather
    /// than to explain it.
    ///
    /// THE WIDTH FOR IT COMES FROM THE INSET, not from the rail. Each layer
    /// costs `2 * bandInsetStep` and pushes the innermost edge right, and the
    /// expander has to stay inside that edge (see `railSideMargin`).
    ///
    /// TAKEN ONE RUNG FURTHER on a design audit's measurement: across the
    /// whole stack the inset moved a region's width by 9px on a 94px rail,
    /// while ONE TINT STEP is visible everywhere at once. Depth is carried by
    /// TONE; the inset only exists so an edge exists to see the tone against,
    /// and 2/3/4/5 gives that edge in a rail 20px narrower than the one
    /// 3/5/7/9 was drawn for. A 1px step is 1.5 device px on a HiDPI screen
    /// and is visible.
    /// THREE, and it was four. The tone ladder now has a ceiling (see
    /// `AppTheme.railNestSurfaces`) because the rail had become the brightest
    /// band in the window, and a shorter ladder is what a dark rail affords:
    /// the total range a receding column can spend is about 2.8:1, and five
    /// rungs inside it are steps nobody can see.
    ///
    /// THE INSET TAKES OVER WHAT THE TONE GAVE UP. Three 1px hairlines at
    /// 1.45:1 were measured reading as "a botched drop shadow" rather than as
    /// nested boxes; 2px steps at a quieter tone read as edges. Bounded by
    /// the expander, which has to sit INSIDE the innermost region: the
    /// deepest inset is `bandInset(maxBandLayers)` and it is the left wall of
    /// the chevron's slot (`chevronSlotLeft`), so the two move together and
    /// neither may be changed alone.
    readonly property int maxBandLayers: 3
    /// Base 1, step 1 — so the ladder is 1/2/3 and the DEEPEST inset is 3.
    /// Previous ladders were 2/4/6 and 1/3/5; each time the deepest inset
    /// moved, it moved the wall the expander is clamped against.
    readonly property int bandInsetBase: AppTheme.scaled(1)
    /// 1, and it was 2. The widening to 2 was made so "the shape carries
    /// what the tone gave up" when the ladder was flattened to ΔL* 3. The
    /// ladder now steps an even 3.4 in LIGHTNESS along the chain that is
    /// actually drawn (AppTheme) rather than evenly in alpha, so tone carries
    /// the nesting again and the inset can hand back the 2px — which is most
    /// of what the expander's plate needed.
    readonly property int bandInsetStep: AppTheme.scaled(1)
    /// Rung 0 is a FOLDER's container, which sits one step OUTSIDE hierarchy
    /// depth 1 because it contains it.
    function bandInset(depth) {
        return bandInsetBase
               + (Math.min(depth, maxBandLayers) - 1) * bandInsetStep
    }
    /// CONCENTRIC, and it was not. The radius stepped by one while the inset
    /// stepped by three, so the corners of two nested regions were not
    /// parallel: the visible band between them pinched from 3px to about 2.4
    /// at every corner. A rounded rectangle inset by N inside another is
    /// concentric only when its radius is smaller by exactly N.
    ///
    /// AND IT WAS A RAW LITERAL. `AppTheme.radiusMd` does not scale, so at
    /// 140% every other thing in this rail grew and the corners did not —
    /// the bands read measurably boxier against the tiles they hold.
    function bandRadius(depth) {
        // MUCH ROUNDER, asked for by name: "round the shapes more around the
        // subspaces". The ladder was 7/5/3, which at a 74px-wide band reads
        // as a rectangle with its corners filed off rather than as a soft
        // container.
        //
        // STILL EXACTLY CONCENTRIC. A rounded rectangle inset by N inside
        // another is concentric only when its radius is smaller by exactly N,
        // and Material names non-concentric nesting as the thing that makes
        // corners look unbalanced — so the ladder steps down by
        // `bandInsetStep`, the same number the inset steps in by, and no
        // other: the radius steps by `bandInsetStep`, exactly as the inset
        // does, which is what keeps them concentric at any ladder.
        return Math.max(AppTheme.scaled(4),
                        AppTheme.scaled(16)
                        - Math.min(depth, maxBandLayers) * bandInsetStep)
    }

    /// Both stops are the tile's own range plus the gutter, so the gutter is
    /// a CONSTANT across the whole range a reader can drag to — which is why
    /// the tile grows in the middle of it and the margins take the rest only
    /// once the tile has stopped.
    readonly property int minRailWidth:
        AppTheme.scaled(40) + 2 * railSideMargin
    readonly property int maxRailWidth:
        AppTheme.scaled(48) + 2 * railSideMargin

    function revealCount(spaceId) {
        if (!app.railLayout || !app.railLayout.spaceExpanded(spaceId))
            return 0
        var explicitCount = railReveal[spaceId]
        return explicitCount ? explicitCount : 5
    }
    function showMoreRooms(spaceId) {
        var next = {}
        for (var k in railReveal)
            next[k] = railReveal[k]
        next[spaceId] = revealCount(spaceId) + 5
        railReveal = next
    }
    // The space's DIRECT joined child rooms, most recently active first — the
    // quick-access reading of "top rooms". Unjoined children are join
    // offers, not rooms this rail can open; they live on Space Home.
    //
    // DIRECT, NOT TRANSITIVE, AND THAT IS THE WHOLE BUG.
    //
    // This called `childRoomsDetailed`, which returns the entire subtree, so
    // every room of every descendant Space was listed under every ancestor
    // tile — a Discord bridge with server spaces and category subspaces drew
    // each channel under its category, its server AND the umbrella above
    // them. Reported 2026-09-17 with a tree diagram; the reporter's words
    // were "the hierarchy is like… out of order".
    //
    // The call site went stale rather than being wrong when written: it was
    // added when the rail had no subspace nesting at all, where transitive
    // was a defensible reading of "what this Space contains". Six days later
    // the rail gained real nesting, and the commit that added it fixed this
    // exact flattening in every other surface and missed this one line. The
    // Channels column is protected from the same mistake by a contract test
    // that bans the accessor BY NAME; the rail had no such test.
    //
    // `directChildRoomsDetailed` had to gain `lastActivity` for this — see
    // SpaceManager.cpp. Without it the sort below silently degrades to state
    // order (the `|| 0` guards keep it from being NaN, which is worse: it
    // would not have looked broken).
    function topRoomsInSpace(spaceId) {
        void spacesRevision
        if (!app.spaces || !spaceId || spaceId.charAt(0) !== "!")
            return []
        var rooms = app.spaces.directChildRoomsDetailed(spaceId)
        rooms.sort(function(a, b) {
            return (b.lastActivity || 0) - (a.lastActivity || 0)
        })
        // THEN THE USER'S ARRANGEMENT, if there is one. Reported right after
        // the subspaces were made draggable — "I can't rearrange rooms inside
        // subspaces, subspaces and spaces work okay" — and it is the same
        // request one level down. Activity order stays the DEFAULT, so a
        // Space nobody has arranged behaves exactly as it always did; the
        // store only reorders what it was told about, and anything it has
        // not heard of keeps its place at the end.
        if (!app.railLayout)
            return rooms
        var ids = []
        for (var i = 0; i < rooms.length; ++i)
            ids.push(rooms[i].roomId)
        var wanted = app.railLayout.orderedRooms(spaceId, ids)
        if (wanted.length !== ids.length)
            return rooms
        var byId = {}
        for (var j = 0; j < rooms.length; ++j)
            byId[rooms[j].roomId] = rooms[j]
        var out = []
        for (var k = 0; k < wanted.length; ++k) {
            if (byId[wanted[k]] !== undefined)
                out.push(byId[wanted[k]])
        }
        return out.length === rooms.length ? out : rooms
    }
    Connections {
        target: app.spaces
        function onSpacesChanged() { root.spacesRevision++ }
    }
    // ── "Show me this Space" ──────────────────────────────────────────
    //
    // The model has already expanded the chain and rebuilt the rows by the
    // time this arrives; all that is left is to put the row on screen. Qt.
    // callLater because the ListView has not laid the new rows out yet —
    // positioning against a count it has not seen scrolls to the wrong place
    // or to nothing at all.
    Connections {
        target: app.railEntries
        function onRevealRequested(spaceId) {
            Qt.callLater(function() {
                if (!app.railEntries)
                    return
                var row = app.railEntries.rowForEntry(spaceId)
                if (row < 0 || row >= list.count)
                    return
                list.positionViewAtIndex(row, ListView.Contain)
            })
        }
    }
    // The expansion lives in the store, so a toggle has to re-evaluate the
    // reveal bindings too.
    Connections {
        target: app.railLayout
        function onLayoutChanged() { root.spacesRevision++ }
    }
    Connections {
        target: app
        function onAccountSwitchingChanged() {
            if (app.accountSwitching)
                root.railReveal = ({})
        }
    }

    // ── Drag state that belongs to the VIEW ──────────────────────────────
    // The order, the drop target and the reorder/group decision all live in
    // the model (see RailEntryModel). What is left here is what only the view
    // can know: where the pointer is, and what the proxy should look like.
    readonly property bool dragging: app.railEntries.dragging
    property real dragViewportY: 0
    // The pointer in CONTENT coordinates. The dragged tile centres itself on
    // this, which is what makes it follow the pointer rather than snap between
    // slots — the model reorder underneath moves its neighbours.
    property real dragContentY: 0

    function beginTileDrag(entryId, sceneY) {
        if (!app.railEntries.beginDrag(entryId))
            return false
        dragViewportY = list.mapFromItem(null, 0, sceneY).y
        dragContentY = dragViewportY + list.contentY
        return true
    }

    // A ROW IS NOT ONE HEIGHT, and this used to assume it was.
    //
    // It returned `normalRowBand` for every row but the first, with the first
    // SAMPLED at the start of a gesture because the handoff divider makes it
    // taller. That was exactly true while every tile was `railTileSize`, and
    // stopped being true when a nested Space's tile became a step smaller
    // (2026-09-18) and the last row of a group started carrying the gap
    // below it: 6px per nested row and 8px per group ABOVE the pointer,
    // which is invisible on a shallow rail and drops a slot off on a deep
    // one. Every row derives its own band now, the divider included, so
    // there is nothing left to sample.
    //
    // Read from the MODEL rather than from the delegate: `rowTop` exists
    // precisely because a delegate's `y` is being interpolated by the move
    // and displaced transitions while a drag is live.
    function rowBand(index) {
        var e = app.railEntries.entryAt(index)
        if (!e)
            return normalRowBand
        var isHome = e.pseudo === true && (e.spaceId || "") === ""
        var isPeople = e.pseudo === true && (e.spaceId || "") === "@people"
        if (peopleTabVisible ? isPeople : isHome)
            return dividerRowBand
        var tile = e.hierarchyChild === true ? railNestedTileSize
                                             : railTileSize
        // The gap belongs to the row whose innermost run ends, matching the
        // delegate's own `trailingGap` — the two are one number and a
        // literal in either place is a mis-drop.
        //
        // `ownsRegion` reduces to "has child ROWS" here, and so does the
        // delegate's: its other arm is `expansionCol.visible`, which is false
        // for the whole of a drag. This function is only ever asked during
        // one, so the two agree exactly when it matters.
        // THE SAME UNCAPPED DEPTH the delegate uses. A capped one disagreed
        // with the delegate on every row past the cap, and this arithmetic is
        // what the drag maps the pointer through.
        var owns = e.bandNextLevel > e.level
        var depth = Math.max(0, e.level) + (owns ? 1 : 0)
        var gap = depth > 0 && e.bandNextLevel >= 0
                  && e.bandNextLevel < depth
                  ? (e.bandNextLevel < 1 ? groupBreakGap : groupGap) : 0
        // AND THE CAP SEAM, which the delegate adds to its own height. This
        // function is what the drag maps the pointer through, so a term in
        // one and not the other is a drop that lands somewhere the reader was
        // not pointing — which is exactly the defect the `ownsRegion` comment
        // above records having shipped once already.
        var seam = groupGap
        var capOpens = owns && depth > maxBandLayers
                       && e.bandPrevLevel >= maxBandLayers
        var capCloses = depth > maxBandLayers
                        && e.bandNextLevel < depth
                        && e.bandNextLevel >= maxBandLayers
        return tile + 2 * rowPad + gap
               + (capOpens ? seam : 0) + (capCloses ? seam : 0)
    }
    // DERIVED from the row heights, not read off `itemAtIndex(i).y`.
    //
    // Every row is exactly its tile band tall while a drag is live — the
    // revealed-rooms columns are hidden for the duration — so accumulating is
    // exact. And it is the only way to be animation-independent: the move and
    // displaced transitions interpolate `y` for 140 ms, so a pointer held
    // still would map to one row, then to its neighbour, then back, and the
    // dragged entry would oscillate between two slots.
    function rowTop(index) {
        var y = 0
        for (var i = 0; i < index && i < list.count; ++i)
            y += rowBand(i) + list.spacing
        return y
    }
    // THE TILE IS THE GROUP TARGET; THE GAP BETWEEN TILES IS THE REORDER
    // TARGET. This is Discord's rule and it is the third attempt at this
    // decision, because the first two were both structurally unreachable.
    //
    //   v1: "the middle 24 px of a row is the group zone". Reaching that
    //   middle means first crossing the row's near edge, which REORDERED —
    //   so the tile being aimed at stepped aside and the row under the
    //   pointer became the dragged entry, which is never a group target.
    //
    //   v2: "short of the row's midpoint you are resting, past it you have
    //   pushed through". The geometry was right and the dispatch was not:
    //   the resting branch ended in `updateDrag(row, !dwellTimer.running)`,
    //   and `running` is TRUE for the whole 250 ms the dwell is being
    //   served — so the second pointer sample inside the target's near half
    //   reordered anyway, and the branch that then fired stopped the very
    //   dwell it was waiting for. Grouping needed a frozen mouse to happen
    //   at all.
    //
    // Both had the same shape: a reading that MOVES THINGS while the user is
    // still aiming. So nothing moves while the pointer is on a tile, full
    // stop. A gesture that never disturbs its own target cannot fail the way
    // those two did, and it needs no dwell to compensate — a pointer sweeping
    // across a tile on the way somewhere else changes the order not at all,
    // which is what the dwell was standing in for.
    //
    // The bands, measured off the TILE and not off the row band: every tile
    // is 40 px at y = 4 inside its row, so a 24 px group band centred on the
    // tile's own centre leaves 12 px of dead space at each end. Between two
    // adjacent 48 px rows that is a 28 px reorder gap (12 + 4 spacing + 12),
    // which is a comfortable target, and it means the visual centre of a tile
    // — where a person aims — is now the middle of the group band instead of
    // sitting exactly on the old boundary.
    readonly property int tileGroupInset: 12
    readonly property int tileGroupBand: 24
    // ONE total, monotone reading of the pointer. Returns either { row: i }
    // (the pointer is on row i's tile) or { gap: g } (the pointer is in the
    // gap before row g; gaps run 0..count, so `count` is the end of the rail).
    function readingAt(contentY) {
        for (var i = 0; i < list.count; ++i) {
            var top = rowTop(i)
            if (contentY < top + tileGroupInset)
                return { gap: i }
            if (contentY < top + tileGroupInset + tileGroupBand)
                return { row: i }
        }
        return { gap: list.count }
    }
    // The dragged block's own slot is the GAP its tile came out of — the tile
    // is drawn under the pointer, not here. There is nothing to group with and
    // nowhere new to move, so a pointer over it holds everything still.
    /// Can the entry currently being dragged be dropped ONTO a tile?
    ///
    /// Only a top-level entry can: a folder is a top-level grouping, so a
    /// subspace has nothing to be filed into, and the model refuses it.
    /// Asking here as well is not a second copy of that rule — it is what
    /// stops the view from sending a gesture somewhere the model will decline
    /// and then doing nothing at all with it.
    function draggedCanGroup() {
        var held = app.railEntries ? app.railEntries.draggingEntryId : ""
        if (!held)
            return false
        var row = app.railEntries.rowForEntry(held)
        if (row < 0)
            return false
        var entry = app.railEntries.entryAt(row)
        return !!entry && entry.hierarchyChild !== true
    }
    function rowIsDraggedBlock(row) {
        var entry = app.railEntries.entryAt(row)
        if (!entry)
            return false
        var held = app.railEntries.draggingEntryId
        return entry.entryId === held
               || (entry.folderId !== undefined && entry.folderId === held)
    }
    // ONE dispatch, called by the pointer AND by the auto-scroll. The
    // auto-scroll used to end in its own unconditional reorder, which meant
    // any auto-scroll step disarmed a grouping the user had just aimed —
    // every 16 ms, for as long as the pointer was within 44 px of an edge.
    function applyPointerReading(contentY) {
        if (!root.dragging)
            return
        var reading = readingAt(contentY)
        // ── A DRAG THAT CANNOT GROUP HAS NO "DO NOTHING" READING ─────────
        //
        // `readingAt` answers "the pointer is ON a tile" or "it is in a gap",
        // and a tile reading means GROUP. A subspace cannot be grouped — a
        // rail folder is a top-level device — so `hoverGroup()` refused it
        // and returned, moving nothing. The tile bands are most of the
        // column's height, so a subspace drag was inert almost everywhere the
        // pointer could be: it lifted, it followed, and it never reordered.
        //
        // For those drags a tile is not a target, it is a POSITION: above its
        // midpoint means before it, below means after. `legalGap()` then
        // snaps that to a boundary between the dragged row's own siblings, so
        // this cannot turn into a reparent.
        if (reading.row !== undefined && !draggedCanGroup()) {
            var rowMid = rowTop(reading.row) + rowBand(reading.row) / 2
            app.railEntries.hoverGap(contentY < rowMid ? reading.row
                                                       : reading.row + 1)
            return
        }
        if (reading.row !== undefined) {
            // A target the pointer has LEFT must stop being lit AND stop
            // being armed: `endDrag` groups on the flag, not on where the
            // pointer is, so a stale one would make a folder out of a
            // release over the gap.
            if (rowIsDraggedBlock(reading.row))
                app.railEntries.clearDropTarget()
            else
                app.railEntries.hoverGroup(reading.row)
            return
        }
        app.railEntries.hoverGap(reading.gap)
    }
    function updateTileDrag(sceneY) {
        if (!root.dragging)
            return
        var local = list.mapFromItem(null, 0, sceneY)
        dragViewportY = local.y
        dragContentY = local.y + list.contentY
        applyPointerReading(dragContentY)
    }

    // Where the dragged tile parks once a release would GROUP: the centre of
    // the target's own row. The tile stops following the pointer and shrinks
    // onto the target, so the two are visibly about to become one thing —
    // and, just as importantly, the full-size dragged tile stops covering the
    // ring that says which Space it would land in. -1 while reordering, where
    // the tile follows the pointer exactly.
    readonly property real groupAnchorY: {
        if (!root.dragging || !app.railEntries.grouping)
            return -1
        var r = app.railEntries.rowForEntry(app.railEntries.dropTargetId)
        return r < 0 ? -1 : rowTop(r) + rowBand(r) / 2
    }

    // Auto-scroll while dragging near either end, so a rail longer than the
    // window does not force the user to drop, scroll and start again.
    Timer {
        id: autoScroll
        interval: 16
        repeat: true
        running: root.dragging && list.contentHeight > list.height
        onTriggered: {
            var zone = 44
            var maxY = Math.max(0, list.contentHeight - list.height)
            var step = 0
            if (root.dragViewportY < zone) {
                // Progressive: the closer to the edge, the faster.
                step = -Math.ceil((zone - root.dragViewportY) / 4)
            } else if (root.dragViewportY > list.height - zone) {
                step = Math.ceil(
                    (root.dragViewportY - (list.height - zone)) / 4)
            }
            if (step === 0)
                return
            var next = Math.max(0, Math.min(maxY, list.contentY + step))
            if (next === list.contentY)
                return
            list.contentY = next
            // The pointer did not move, but the row under it did — so this
            // goes through the SAME dispatch a pointer move does. It used to
            // end in its own unconditional reorder, which cancelled an armed
            // grouping every 16 ms near either edge.
            root.dragContentY = root.dragViewportY + list.contentY
            root.applyPointerReading(root.dragContentY)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        // SYMMETRIC. The `+ 2` here made the rail's top inset 18 against a
        // bottom of 12 — "top and bottom ui is not centered", vertically.
        anchors.topMargin: AppTheme.spacing12
        anchors.bottomMargin: AppTheme.spacing12
        spacing: 0

        ListView {
            id: list
            objectName: "spacesRailList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // AIR BEFORE THE DIVIDER. There was none: a scrolled rail clipped
            // its last tile flat, mid-monogram, with the bottom cluster's
            // divider jammed against the cut edge — measured at 0px, against
            // 10px of clearance on the divider at the top of the rail.
            // AND ENOUGH OF IT THAT THE FADE ONLY EVER COVERS EMPTY
            // SUBSTRATE. At `railTileGap` the fade's 16px overlapped the last
            // tile by 5, dimming its bottom edge ~25% — a tile that looks
            // faulty rather than a column that looks scrollable.
            Layout.bottomMargin: AppTheme.scaled(16)
            // A real model, so a preview reorder is a MOVE and not a reset.
            model: app.railEntries
            clip: true
            spacing: AppTheme.spacing4
            // Recycling is off on purpose: a rail holds a handful of rows, and
            // a recycled delegate mid-drag is how the gesture loses its own
            // tile.
            reuseItems: false

            // NO SCROLLBAR. A 78px column of round tiles does not have room
            // for a rail-length vertical bar beside them, and what it drew
            // was a hard grey line down the one edge every region boundary
            // meets — "ugly as hell", and the fade below already says the
            // column continues. The wheel, a drag and the keyboard all still
            // scroll it; Discord's rail makes the same call.

            // ── The bottom fade ──────────────────────────────────────────
            //
            // A guillotined tile is the loudest "this is broken" artefact a
            // scrolling column can produce, and it is also the ONLY thing
            // here that says the rail scrolls at all — there is no persistent
            // scrollbar and no other indicator. A partly-scrolled tile
            // dissolves into the rail instead of being cut flat.
            //
            // A direct child of the ListView, NOT of its contentItem: a child
            // of the content scrolls with it and the fade would slide away
            // from the edge it exists to soften.
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: AppTheme.scaled(16)
                z: 5
                visible: list.contentHeight > list.height
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 1.0; color: AppTheme.rail }
                }
            }

            // What makes the rearrangement read as movement rather than as a
            // jump. `displaced` covers the rows the moved one pushed past.
            move: Transition {
                NumberAnimation {
                    properties: "y"
                    duration: 140
                    easing.type: Easing.OutCubic
                }
            }
            displaced: Transition {
                NumberAnimation {
                    properties: "y"
                    duration: 140
                    easing.type: Easing.OutCubic
                }
            }

            delegate: Item {
                id: spaceItem
                required property int index
                required property string entryId
                required property string kind
                required property string spaceId
                required property string name
                required property string avatarUrl
                required property int unreadTotal
                required property int highlightTotal
                required property int level
                required property string folderId
                required property bool collapsed
                required property int childCount
                required property var memberPreview
                required property bool pseudo
                required property bool hierarchyChild
                required property bool expandable
                required property bool expanded
                required property bool dragged
                required property bool dropTarget
                required property bool folderLast
                required property bool draggable
                // DECLARED, or they are `undefined` and everything that
                // reads them quietly takes the other branch — QML does not
                // complain about an unknown property on a delegate, it just
                // hands back undefined. Both of these shipped unread for
                // several hours: the field's corner-squaring children were
                // `!undefined` (so a run was always square at both ends) and
                // the group gap was never added. Caught by a geometric case
                // comparing `rowTop()` against the delegates, not by looking.
                required property int bandPrevLevel
                required property int bandNextLevel

                readonly property bool isFolder: kind === "folder"
                readonly property bool isHome: pseudo && spaceId === ""
                readonly property bool isPeople: pseudo && spaceId === "@people"
                readonly property bool isRealSpace:
                    !pseudo && !isFolder && spaceId.charAt(0) === "!"
                readonly property bool inFolder:
                    folderId.length > 0 && !isFolder

                width: list.width
                // 48 = 40px tile + 4px on each side so the active accent
                // outline (drawn at -4px margins) is never clipped by the
                // list bounds — this was the Home-icon clipping defect.
                // Home carries the handoff divider (32×2) below its tile.
                // The handoff divider sits under the LAST navigation tab, so
                // it separates the tabs from the Spaces rather than splitting
                // the tabs from each other.
                readonly property bool carriesDivider:
                    root.peopleTabVisible ? isPeople : isHome
                // SCALED, WITH THE TILE IT HOLDS. Measured on a real build:
                // the rail widened from 112 to 152 between 100% and 140%
                // interface and the tile stayed exactly 38px of colour — all
                // the extra width went into the gutters. Every calculation
                // around it already used `railTileSize`, which IS scaled, so
                // the drawn tile was the one thing that did not move. The
                // band is `tile + 8` (4px above and below) and the divider
                // row adds its own 10.
                //
                // NOTE for the next reader: the 2026-09-17 note on this file
                // said "at 140% the tiles grew and the indent did not". The
                // tiles did not grow either; both were unscaled, and only
                // the indent was fixed at the time.
                readonly property int tileBandHeight:
                    carriesDivider
                    ? root.dividerRowBand
                    : spaceItem.rowTileSize + 2 * root.rowPad
                /// Anything nested is drawn one step smaller, at any depth.
                readonly property int rowTileSize:
                    spaceItem.hierarchyChild ? root.railNestedTileSize
                                             : root.railTileSize
                /// How many nested regions this row draws, capped: one per
                /// ancestor, plus its own when it owns the run below it.
                readonly property int bandLayers:
                    Math.min(root.maxBandLayers,
                             Math.max(0, spaceItem.level)
                             + (spaceItem.ownsRegion ? 1 : 0))
                /// Which rung of the ladder the INNERMOST region on this row
                /// is painted with — the surface the expander's plate sits
                /// on, and therefore the one it has to step up from. A row
                /// with no region at all sits on the rail itself, and it
                /// counts as rung 0 here for one reason: +2 from rung 0 is
                /// L* 11.78 against the rail's 4.95, the SAME 6.8 ΔL* step
                /// the plate gets over every region. Counting it as -1 made
                /// a collapsed Space's plate 3.4 above the rail and an
                /// expanded one's 6.8 above its region — a control that
                /// changes weight with the state of the thing it toggles,
                /// which is the same complaint as the box that changed size.
                readonly property int innermostTint:
                    spaceItem.bandLayers > 0
                    ? spaceItem.bandTint(spaceItem.bandLayers - 1) : 0
                /// Does a run hang off this tile — subspaces as model rows,
                /// or rooms revealed inside this delegate?
                readonly property bool ownsRegion:
                    spaceItem.bandNextLevel > spaceItem.level
                    || expansionCol.visible
                /// How deep this row's own region really is, uncapped.
                readonly property int trueBandDepth:
                    Math.max(0, spaceItem.level)
                    + (spaceItem.ownsRegion ? 1 : 0)
                /// ── NO TWO REGIONS THAT TOUCH WEAR ONE TINT ───────────
                ///
                /// The stack is capped, so every row past the cap draws its
                /// own region as "the innermost layer" — which meant a
                /// depth-5 region was drawn directly inside a depth-4 one at
                /// the same inset AND the same tint, and the two were the
                /// same picture. Asked in those words: "are these supposed to
                /// be the same color?" They were, and the honest answer is
                /// that the cap had stopped distinguishing.
                ///
                /// Past the cap the innermost layer ALTERNATES between the
                /// last two rungs, so a parent and the child drawn on top of
                /// it always differ. The ancestor layers are untouched, which
                /// is what keeps the alternation from colliding with the
                /// depth-(cap-1) region on the same row.
                function bandTint(depth) {
                    if (depth < spaceItem.bandLayers)
                        return depth
                    var overflow = spaceItem.trueBandDepth - root.maxBandLayers
                    if (overflow <= 0)
                        return depth
                    return root.maxBandLayers + (overflow % 2)
                }
                /// The air after the last row of a run, added by that row.
                ///
                /// KEYED ON THE ROW'S INNERMOST REGION, and it used to be
                /// keyed on depth 1 — the top-level group. That left every
                /// INNER run butting straight into the next one with only
                /// `ListView.spacing` between them: reported as "the lowest
                /// level runs out of color, there are small gaps between them
                /// where the color should end, we want to separate it
                /// cleanly", with two runs of the same tint reading as one
                /// shape with a hairline notch through it.
                ///
                /// The gap is not a hole in the parent, because a layer whose
                /// own run continues BRIDGES it (see the Repeater's height).
                /// So what a reader sees between two sibling runs is the
                /// parent's tint, at full height, which is the separation.
                /// KEYED ON THE TRUE DEPTH, not the capped one, and that
                /// distinction is a real defect the capped version shipped.
                ///
                /// `bandLayers` stops at `maxBandLayers`, so a row at depth 7
                /// followed by one at depth 4 compared 4 < 4 and spent NO
                /// gap — while the tint alternation, which only ever promised
                /// that a parent differs from its child, put both regions on
                /// the same rung. Measured: a deep-level-8 run and a
                /// burst-space run rendered as one unbroken `#91969D` with no
                /// boundary pixel between them, at the same inset.
                ///
                /// The uncapped depth restores the guarantee the cap broke:
                /// two regions can still share a tint, and they can no longer
                /// TOUCH while doing it.
                /// ── THE SEAM PAST THE DEPTH CAP ────────────────────────
                ///
                /// Reported as "these should be rounded", of a hard square
                /// edge between two bands. A design study measured it and
                /// found a case none of the existing rules covers: a child
                /// region OPENING PAST THE CAP. Past `maxBandLayers` the
                /// child cannot be inset any further, so parent and child sit
                /// at the SAME inset with no gap, and the only thing telling
                /// them apart is 1.33:1 of tone — at a full-bleed straight
                /// edge, which reads as a fold in one surface rather than as
                /// one thing inside another. Worse, the tone alternation is
                /// non-monotonic: sometimes the deeper band is lighter,
                /// sometimes darker, so tone cannot even say which side is
                /// inside.
                ///
                /// A RADIUS ALONE WOULD NOT HAVE FIXED IT, which is why this
                /// is not what was asked for. Two 3px corner notches are
                /// 3.9px² out of a 66x41 band — invisible — and the radius
                /// cannot grow, because the enclosing band is 2px outside it
                /// and concentricity is what keeps nested corners from
                /// looking unbalanced. Worse still, rounding alone fills the
                /// notch with whatever is BEHIND, which is the GRANDPARENT's
                /// band at 1.73:1 — the largest step in the ladder, at the
                /// boundary that should show the smallest.
                ///
                /// So the seam is AIR IN THE PARENT'S OWN TONE, and the
                /// radius is what keeps that air from reading as a slot. The
                /// resulting vocabulary has two boundaries that can never be
                /// confused: a run ENDS and shows 8px of the grandparent;
                /// a child BEGINS and shows 3px of the parent.
                /// The seam is the AIR the corner turns in, not the radius
                /// itself. It was written as `bandRadius(maxBandLayers)` when
                /// that was 3; the radii are 14/12/10 now and a ten-pixel
                /// notch between every over-cap parent and child would be a
                /// gap, not a seam — and it would compete with the 8px a run
                /// that genuinely ENDS already spends.
                /// 8, and it was 4. THE ROUNDING ONLY READS WHERE THERE IS
                /// BACKGROUND BEHIND THE CORNER, and at 4 there was not:
                /// consecutive regions stacked all but flush, so of 24
                /// corners in a deep run only 4 met open rail and the other
                /// 20 were a corner curving in meeting one curving out. The
                /// radii went to 14/12/10 and bought twenty pinches.
                ///
                /// Eight is the same air a run that ENDS already spends, so
                /// every boundary between two regions is now one number and
                /// the three different junction treatments a critique
                /// measured collapse to one.
                readonly property int capSeamSize: root.groupGap
                /// Guarded on the parent's band already being present at this
                /// inset, so a cap seam can never stack with the gap a run
                /// that truly ends already spends.
                readonly property bool capOpens:
                    spaceItem.ownsRegion
                    && spaceItem.trueBandDepth > root.maxBandLayers
                    && spaceItem.bandPrevLevel >= root.maxBandLayers
                readonly property bool capCloses:
                    spaceItem.trueBandDepth > root.maxBandLayers
                    && spaceItem.bandNextLevel < spaceItem.trueBandDepth
                    && spaceItem.bandNextLevel >= root.maxBandLayers
                readonly property int capSeamTop: capOpens ? capSeamSize : 0
                /// WHERE THIS ROW'S CONTENT STARTS. The cap seam moves the
                /// BAND down, and everything drawn on the band has to move
                /// with it: the tile, its chevron, its revealed rooms. It did
                /// not, so on a row that opens a seam the tile stayed at the
                /// row's top and stuck out through the top edge of its own
                /// region — reported as "blue DL looks very bad, the whole
                /// region". A shape outside the shape that contains it.
                readonly property int contentTop: capSeamTop
                readonly property int capSeamBottom:
                    capCloses ? capSeamSize : 0
                readonly property int trailingGap:
                    spaceItem.trueBandDepth > 0
                    && spaceItem.bandNextLevel >= 0
                    && spaceItem.bandNextLevel < spaceItem.trueBandDepth
                    ? (spaceItem.bandNextLevel < 1 ? root.groupBreakGap
                                                   : root.groupGap)
                    : 0
                // The seam comes out of the ROW, not out of the band: there
                // are only `rowPad` (4) pixels of band above a tile, and
                // taking 3 of them would leave the band clipping the tile it
                // is drawn for.
                height: tileBandHeight
                        + (expansionCol.visible ? expansionCol.height + 2 : 0)
                        + spaceItem.trailingGap
                        + spaceItem.capSeamTop + spaceItem.capSeamBottom

                property bool isActive: app.spaces && !isFolder
                                        && app.spaces.activeSpaceId === spaceItem.spaceId
                // How far the tile is lifted from its own slot to sit under
                // the pointer. Zero for every row but the one being dragged,
                // so nothing else pays for it.
                readonly property real dragLift:
                    !spaceItem.dragged
                    ? 0
                    : (root.groupAnchorY >= 0 ? root.groupAnchorY
                                              : root.dragContentY)
                      - (spaceItem.y + spaceItem.tileBandHeight / 2)
                // Above its neighbours while it travels over them.
                z: spaceItem.dragged ? 10 : 0
                // Indentation: a filed Space steps in a little, a subspace
                // steps in per level, and the whole thing is clamped to
                // whatever the rail's CURRENT width can carry.
                //
                // The clamp used to be the literal 14, which was right for
                // exactly one rail width because the rail had exactly one
                // width. Now it is `root.indentBudget`, so widening the rail
                // reveals more depth and narrowing it back hides it again,
                // with no mode to switch and no level to count.
                // GATED ON THE `expanded` ROLE, NOT ON A FUNCTION CALL.
                //
                // `revealCount()` asks `app.railLayout.spaceExpanded(id)`,
                // which is a Q_INVOKABLE — so a binding that reaches the
                // expansion state only through it records NO dependency on
                // it and never re-evaluates when it changes. Expanding a
                // Space that HAS subspaces inserts model rows, which rebuilds
                // the delegate and hides that; a LEAF category inserts none,
                // so its chevron flipped open (that reads the role, which
                // does update) and its rooms stayed hidden until an unrelated
                // toggle or an app restart rebuilt the rail.
                //
                // Reported as "a room did not appear under its space until
                // Lightning restarted" and filed as sync staleness. It was
                // not: Space Home listed the rooms the whole time. Same shape
                // as `root.info` in Space settings — a CALL where a binding
                // was needed.
                readonly property int revealed:
                    (isRealSpace && spaceItem.expanded)
                        ? root.revealCount(spaceItem.spaceId) : 0
                readonly property var revealedRooms:
                    revealed > 0 ? root.topRoomsInSpace(spaceItem.spaceId) : []
                /// The revealed rooms as the CURRENT GESTURE has arranged
                /// them, or null when no gesture is live. See the Repeater
                /// below: while a drag is running this is the model, so the
                /// arrangement a reader sees is the one the release writes.
                property var roomPreview: null
                property int roomDragIndex: -1
                /// Moves the dragged room to `to` in the preview. Clamped,
                /// so a pointer dragged past either end parks at that end
                /// rather than falling out of the list.
                function moveRoomPreview(to) {
                    if (!spaceItem.roomPreview || spaceItem.roomDragIndex < 0)
                        return
                    var list = spaceItem.roomPreview
                    var target = Math.max(0, Math.min(list.length - 1, to))
                    if (target === spaceItem.roomDragIndex)
                        return
                    var next = list.slice()
                    next.splice(target, 0, next.splice(
                        spaceItem.roomDragIndex, 1)[0])
                    spaceItem.roomDragIndex = target
                    // A NEW ARRAY, not a mutation. QML compares `var`
                    // properties by reference, so splicing the bound list in
                    // place changes what the Repeater reads and tells it
                    // nothing — the rail would only redraw on the next
                    // unrelated update.
                    spaceItem.roomPreview = next
                }
                function commitRoomOrder() {
                    var list = spaceItem.roomPreview
                    spaceItem.roomPreview = null
                    spaceItem.roomDragIndex = -1
                    if (!list || !app.railLayout || !spaceItem.spaceId)
                        return
                    var ids = []
                    for (var i = 0; i < list.length; ++i)
                        ids.push(list[i].roomId)
                    app.railLayout.setRoomOrder(spaceItem.spaceId, ids)
                }

                Accessible.role: Accessible.Button
                Accessible.name: isFolder
                                 ? qsTr("Folder: %1").arg(spaceItem.name)
                                 : isHome ? qsTr("All rooms")
                                 : isPeople ? qsTr("Direct Messages")
                                 : spaceItem.spaceId === "@orphans"
                                   ? qsTr("Other rooms") : spaceItem.name

                // ── The open folder's container ─────────────────────────────
                // One surface behind the header and its members, which is the
                // whole difference between a folder and several adjacent
                // Spaces. Drawn per row and squared off between them, so the
                // run reads as continuous however long it is.
                Rectangle {
                    visible: (spaceItem.isFolder && !spaceItem.collapsed)
                             || spaceItem.inFolder
                    // ── ON THE SAME LADDER AS THE HIERARCHY REGIONS ──
                    //
                    // It was not, and the two defects that came out of that
                    // were both invisible to every check here.
                    //
                    // FIRST, `z: -2` put this container ON TOP of the region
                    // stack (z -21..-17), in a colour those regions already
                    // used — so a Space tree filed into a folder was painted
                    // flat and lost its nesting entirely. Measured on a
                    // capture: inside a folder, exactly ONE region tint
                    // appeared in the whole rail. Nothing was wrong with the
                    // model; the picture was drawn over.
                    //
                    // SECOND, the margins were a RAW 6 against a ladder in
                    // SCALED units — 6 lands between depth 1 and depth 2, so
                    // a container was drawn NARROWER than the regions it
                    // contains, and the boundary between two groups became
                    // four corner arcs and a hairline inside 20px.
                    //
                    // A folder and a hierarchy region say the same thing, so
                    // they are one device: this is rung 0, outside depth 1,
                    // behind everything.
                    anchors.left: parent.left
                    anchors.right: parent.right
                    objectName: "railFolderContainer"
                    anchors.leftMargin: root.bandInset(0)
                    anchors.rightMargin: root.bandInset(0)
                    y: 0
                    height: spaceItem.isFolder
                            ? spaceItem.tileBandHeight + list.spacing
                            : spaceItem.height
                              + (spaceItem.folderLast ? 0 : list.spacing)
                    z: -22
                    color: AppTheme.railNestSurfaces[0]
                    radius: root.bandRadius(0)
                    // Square off the joins so the container is one shape.
                    Rectangle {
                        visible: spaceItem.isFolder
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.radius
                        color: parent.color
                    }
                    Rectangle {
                        visible: spaceItem.inFolder
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: parent.radius
                        color: parent.color
                    }
                    Rectangle {
                        visible: spaceItem.inFolder && !spaceItem.folderLast
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.radius
                        color: parent.color
                    }
                }

                // Active outline: 2 px accent ring offset from the tile.
                // The offset is `tileRingOutset` and NOT a literal, because
                // the chevron's placement is measured from this ring's outer
                // edge — see that property. Two numbers, one fact.
                Rectangle {
                    objectName: "railSpaceActiveRing"
                    anchors.fill: spaceTile
                    anchors.margins: -root.tileRingOutset
                    // CONCENTRIC: a rounded rect outset by N is concentric
                    // with its tile only when its radius is larger by exactly
                    // N. It was +3 against an outset of 4.
                    radius: root.railTileRadius + root.tileRingOutset
                    color: "transparent"
                    border.color: AppTheme.accent
                    border.width: 2
                    visible: spaceItem.isActive
                }

                // Handoff divider between Home and the Space tiles: a rule
                // four fifths of a tile wide, so it stays narrower than the
                // tiles it separates at every interface size.
                Rectangle {
                    visible: spaceItem.carriesDivider
                    // CENTRED UNDER THE TILE, and it was `x: tileColumnX` at
                    // 80% of the tile's width — flush with the tile's LEFT
                    // edge and 20% short on the right, centred under nothing.
                    // Shares its rule with the bottom cluster's separator so
                    // the two read as one device used twice.
                    width: root.railDividerWidth
                    height: 2; radius: 1
                    color: AppTheme.border
                    x: root.railDividerX
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                }

                // ── The group field ─────────────────────────────────────
                //
                // What a run of nested rows sits ON, instead of what used to
                // be drawn BETWEEN them. Common region: the Gestalt cue that
                // needs no horizontal space, which is the whole reason it is
                // the one every narrow rail converges on — Discord tints a
                // pill behind a folder's servers, and this rail's own folder
                // container has done exactly this since it shipped.
                // ── WHAT THE CAP SEAM SHOWS ─────────────────────────────
                //
                // The parent's own tone, and this rectangle is the only thing
                // that can supply it. The layer stack draws ONE region per
                // depth, so behind the cap layer sits the depth-(cap-1) band
                // — the GRANDPARENT. Open a seam without this and the air
                // fills with rung 2 against rung 4: 1.73:1, the largest step
                // in the ladder, appearing at the one boundary that should
                // show the smallest. It would say "grandparent" exactly where
                // the picture is trying to say "one deeper".
                //
                // Its own objectName, deliberately: `railGroupField` is
                // counted by a geometric case that requires each layer to be
                // narrower than the one containing it, and this one is at the
                // SAME inset by construction. A fractional z rather than an
                // equal one, because same-z siblings paint in document order
                // and that is not a thing to rely on twice.
                Rectangle {
                    objectName: "railCapBackdrop"
                    // ── VISIBLE WHENEVER THE ROW IS PAST THE CAP ─────
                    //
                    // It was gated on the SEAM, and the seam is gated on the
                    // row above already being at the cap — so in the common
                    // case, a parent shallower than the cap owning a child at
                    // it, the backdrop was never drawn. The cap layer's
                    // rounded corners then exposed whatever was behind them,
                    // which is the GRANDPARENT's band: measured on a capture
                    // as two rows where x 5..8 reads the depth-2 tone instead
                    // of the depth-3 one. Reported as a stray corner beside
                    // the teal and purple tiles, and it is exactly the notch
                    // this rectangle exists to fill.
                    //
                    // The seam and the backdrop are different questions. The
                    // seam asks "is there air here"; the backdrop asks "what
                    // colour is behind this child's corners", and the answer
                    // to the second is "its parent" on every row past the
                    // cap, air or no air.
                    visible: spaceItem.trueBandDepth > root.maxBandLayers
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: root.bandInset(root.maxBandLayers)
                    anchors.rightMargin: root.bandInset(root.maxBandLayers)
                    y: 0
                    height: spaceItem.height - spaceItem.trailingGap
                            + (spaceItem.bandNextLevel >= root.maxBandLayers
                               ? list.spacing + spaceItem.trailingGap : 0)
                    z: -20 + root.maxBandLayers - 0.5
                    // ROUNDED WHERE THE PARENT'S RUN ACTUALLY ENDS.
                    //
                    // This was a flat `radius: 0`, on the reasoning that the
                    // backdrop only ever stands in for a parent whose run
                    // CONTINUES through the row. That reasoning does not match
                    // its own visibility: it is drawn whenever the row is past
                    // the cap, which says nothing about whether the parent
                    // continues — and on the LAST row of an over-cap run the
                    // parent ends exactly here, so a radius-0 rectangle left a
                    // hard square corner under the child's rounded one.
                    // Reported 2026-09-19 with a photograph of that corner.
                    //
                    // So it rounds like every other band and squares off the
                    // end that CONTINUES, which is the same device the layers
                    // above use — `bandPrevLevel`/`bandNextLevel` still at or
                    // past the cap means the parent's run carries on through
                    // that edge and there is nothing to round there.
                    radius: root.bandRadius(root.maxBandLayers)
                    // THE OTHER PARITY — the rung the child is not wearing.
                    color: AppTheme.railNestSurfaces[
                        Math.min(AppTheme.railNestSurfaces.length - 1,
                                 root.maxBandLayers
                                 + ((spaceItem.trueBandDepth
                                     - root.maxBandLayers + 1) % 2))]
                    Rectangle {
                        visible: spaceItem.bandPrevLevel >= root.maxBandLayers
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: parent.radius
                        color: parent.color
                    }
                    Rectangle {
                        visible: spaceItem.bandNextLevel >= root.maxBandLayers
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.radius
                        color: parent.color
                    }
                }

                Repeater {
                    // ── ONE REGION PER ANCESTOR, OUTERMOST FIRST ────────
                    //
                    // `model` is the row's own depth, so a depth-3 row draws
                    // three rectangles: its top-level Space's region, its
                    // parent's inside that, and its own inside that. The
                    // parent's region therefore runs unbroken behind every
                    // descendant instead of being replaced by the deeper
                    // one's tint, which is what made three runs under one
                    // Space read as three unrelated bands.
                    //
                    // NOT hidden during a drag, and the lanes this replaced
                    // were. `stampGroupField` runs inside `applyRows`, the
                    // one chokepoint every row set passes through — the drag
                    // PREVIEW included — so the layers a reader sees
                    // mid-gesture are the layers the release will produce.
                    // That is the whole answer to "can these be rearranged
                    // cleanly": the drop target is drawn, not imagined.
                    // …AND THE OWNER IS INSIDE ITS OWN REGION. A row at
                    // depth L draws L layers as a member, plus ONE MORE when
                    // it is the tile that owns the run below it — so the
                    // region reads as "this Space and everything in it"
                    // rather than as a band that begins under it. A
                    // top-level Space is at depth 0 and draws exactly that
                    // one, which is why an expanded Space is boxed and a
                    // collapsed one sits on bare rail.
                    model: Math.min(root.maxBandLayers,
                                    Math.max(0, spaceItem.level)
                                    + (spaceItem.ownsRegion ? 1 : 0))
                    delegate: Rectangle {
                        id: bandLayer
                        objectName: "railGroupField"
                        required property int index
                        readonly property int depth: index + 1
                        // THE INNERMOST DRAWN LAYER SPEAKS FOR EVERYTHING
                        // BELOW IT. Past the cap a region would be narrower
                        // than the tile it contains, so a depth-6 row joins
                        // the depth-4 layer rather than getting two nobody
                        // can see — and its bounds are then "depth >= 4",
                        // which is what `bandDepth` says.
                        readonly property int bandDepth:
                            depth === root.maxBandLayers
                            ? root.maxBandLayers : depth
                        // Compared against the NEIGHBOURS' depths, per layer.
                        // A pair of booleans on the row could only ever have
                        // described the innermost one.
                        //
                        // The owner's layer opens ON THE OWNER, by
                        // definition. For every other layer the row above is
                        // either another member (deeper or equal) or this
                        // region's own owner (exactly one shallower), and
                        // neither of those is an opening — so a layer opens
                        // only where the row above is outside it altogether.
                        // IT MUST SURVIVE THE CAP. `depth === level + 1` is
                        // a depth the cap has already clipped away for an
                        // over-cap owner, so its layer never opened and was
                        // squared off instead — which is the seam.
                        readonly property bool isOwnerLayer:
                            spaceItem.ownsRegion
                            && depth === Math.min(spaceItem.trueBandDepth,
                                                  root.maxBandLayers)
                        readonly property bool opensHere:
                            isOwnerLayer
                            || spaceItem.bandPrevLevel < bandDepth - 1
                        // TRUE DEPTH ON THE CAP LAYER. Comparing the capped
                        // one is the same mistake `trailingGap` records
                        // having shipped once: a row deeper than the cap
                        // compares its neighbour against a number the cap has
                        // flattened, and never closes.
                        readonly property bool closesHere:
                            spaceItem.bandNextLevel
                            < (depth === root.maxBandLayers
                               ? spaceItem.trueBandDepth : bandDepth)
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: root.bandInset(depth)
                        anchors.rightMargin: root.bandInset(depth)
                        // THE CAP LAYER STARTS BELOW THE SEAM. Every other
                        // layer still starts at the row's top — the seam
                        // belongs to the child that opens, not to the parent
                        // it opens inside.
                        y: depth === root.maxBandLayers
                           ? spaceItem.capSeamTop : 0
                        // PLUS THE LIST'S OWN SPACING while this layer's run
                        // continues, or the layer is not one shape at all:
                        // `ListView.spacing` puts 4px of rail background
                        // between consecutive delegates, and a per-row
                        // rectangle that stops at its own delegate leaves
                        // that seam showing through the middle of the group.
                        // Measured on a capture — two bands with a 4px dark
                        // line between them — not reasoned about.
                        // …AND THE TRAILING GAP TOO, for a layer whose own
                        // run carries on past it: the gap belongs to the run
                        // that ENDED, so a region still open across it has to
                        // cover it or the parent gets a notch where its child
                        // happened to stop.
                        height: spaceItem.height - spaceItem.trailingGap
                                - (depth === root.maxBandLayers
                                   ? spaceItem.capSeamTop
                                     + spaceItem.capSeamBottom : 0)
                                + (closesHere
                                   ? 0
                                   : list.spacing + spaceItem.trailingGap)
                        // Outermost furthest back, so each layer is drawn ON
                        // the one containing it.
                        z: -20 + depth
                        radius: root.bandRadius(depth)
                        // INDEX `depth`, NOT `depth - 1`. Rung 0 of the
                        // ladder is a FOLDER's container, which sits outside
                        // hierarchy depth 1 — so reading it here shifted the
                        // whole ramp one rung down and made the first
                        // boundary the quietest instead of the evenest.
                        // Caught by measuring a capture, not by reading this
                        // line, which is correct-looking either way.
                        color: AppTheme.railNestSurfaces[
                            Math.min(AppTheme.railNestSurfaces.length - 1,
                                     spaceItem.bandTint(depth))]
                        Rectangle {
                            // Square off the top when this layer's run
                            // continues above, so a run of any length reads
                            // as one shape.
                            visible: !bandLayer.opensHere
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: parent.radius
                            color: parent.color
                        }
                        Rectangle {
                            visible: !bandLayer.closesHere
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: parent.radius
                            color: parent.color
                        }
                    }
                }

                // The expander: a quiet tree glyph living ENTIRELY in the
                // gutter left of the tile, never touching the active accent
                // outline. Right-pointing when closed, down when open — the
                // tree convention. It reveals BOTH the Space's subspaces (as
                // real hierarchy rows, inserted by the model) and its top
                // rooms, which is the whole of what the Space contains.
                Item {
                    id: expandChevronArea
                    objectName: "railSpaceExpandChevron"
                    // ── ON THE TILE, not in the gutter ──────────────────
                    //
                    // The gutter belongs to the lanes now, and a lane is four
                    // to ten pixels wide — a twelve-pixel glyph cannot stand
                    // in one without covering its neighbours. It also has no
                    // business there: the lanes say what CONTAINS this Space,
                    // and the chevron says what this Space is DOING, which is
                    // a fact about the tile.
                    //
                    // So it is a badge in the tile's bottom-left corner, at
                    // the same place on every tile at every depth. It is
                    // PERMANENT for a Space with something to open — hover-only
                    // was tried for one revision and the report was immediate,
                    // "I don't see how to collapse it" — and it never touches a
                    // line, so the tree behind it stays unbroken.
                    //
                    // ONLY WHEN THERE IS SOMETHING TO EXPAND. It used to
                    // appear on hover over ANY real Space, so a Space with no
                    // joined subspaces offered a control that opened nothing
                    // — reported as "remove this small arrow left of space,
                    // it does nothing now". `expandable` is the model's own
                    // answer (childSpaceCount > 0) and was already computed;
                    // the chevron simply never read it.
                    visible: spaceItem.isRealSpace && spaceItem.expandable
                             && !root.dragging
                    // ── IN THE GUTTER, AND THE TILE IS LEFT WHOLE ───────
                    //
                    // Third home, and the first one that costs nothing. A
                    // notch straddling the tile's corner was measured biting
                    // a 10x14px hole out of a 40px tile and filling it with
                    // rail background — a quarter of that corner, on ten of
                    // the twenty tiles, so the column's own silhouette came
                    // out eroded. Its disc was 1.07:1 against the rail, which
                    // is to say invisible, and the only thing anyone could
                    // actually see was a 5x3px tick sitting in a dent.
                    //
                    // The gutter is empty now that nothing is drawn in it, so
                    // the mark that lost to the lanes wins by default. It has
                    // the gutter to itself, so it keeps ONE x at every depth
                    // and needs no disc: there is no longer a line for it to
                    // interrupt, and nothing for it to be read against.
                    width: root.tileColumnX
                    height: spaceItem.tileBandHeight
                    x: 0
                    y: spaceItem.contentTop
                    Icon {
                        id: expandGlyph
                        objectName: "railSpaceExpandGlyph"
                        // PLACED BY ITS INK. Right-anchoring the BOX spent a
                        // quarter of the gutter on the font's own empty side
                        // bearing and left the visible mark 9px from the tile
                        // it acts on — a gap 2.7x the mark's own width, which
                        // is why it read as debris rather than as a control.
                        //
                        // `width` here is the glyph's own ADVANCE, and the
                        // ink is centred in it. Not a binding loop: an
                        // Icon's width comes from its font metrics and does
                        // not depend on where it is put.
                        // Tracks its tile while the rail is wide enough to
                        // let it, and clamps into the innermost region when
                        // it is not. `width` is the glyph's ADVANCE and the
                        // ink is centred in it, so the box is offset by half
                        // the side bearing to put the INK where this says.
                        // NOT ROUNDED. The advance carries a fractional side
                        // bearing, so rounding the BOX moves the INK by that
                        // much — and the gutter is exactly full, so rounding
                        // moved it straight back out of its own region.
                        // Placing the ink is the whole point of this
                        // expression; round it and it is placing the box.
                        x: root.chevronInkLeft
                           - (width - root.chevronInkWidth) / 2
                        anchors.verticalCenter: parent.verticalCenter
                        // BOTH glyphs sit high in their own box and they do
                        // not sit high by the same amount — measured in the
                        // square plate: `expand_more` needed 1.33 and
                        // `chevron_right` 0.67 of the rise to centre. NOT
                        // rounded, for the reason x is not: a rounded offset
                        // is a whole pixel of error on a 12px control.
                        anchors.verticalCenterOffset:
                            (spaceItem.expanded ? 1.33 : 0.67)
                            * root.chevronInkRise
                        // ONE MEANING: open or closed. A separate glyph for
                        // "this one dives" was tried twice and rejected both
                        // times — as `chevron_right` it was indistinguishable
                        // from "collapsed", and as an arrow it read as a stray
                        // mark in the gutter.
                        name: spaceItem.expanded ? "expand_more"
                                                 : "chevron_right"
                        size: root.chevronGlyphSize
                        // STEPPED WITH THE REGION UNDER IT. The glyph's
                        // colour was constant while the region it sits on
                        // gets a tint step lighter each level, so the same
                        // control measured 4.37:1 at depth 1 and 3.01:1 at
                        // depth 3 — a third of its contrast lost, at the
                        // depth where the rail is busiest. It moves with its
                        // background now, in whichever direction the theme
                        // takes: `text` is dark on a light preset, so the
                        // same tint darkens there.
                        // ONE INK, and it used to step with the band under
                        // it. That was the right answer to a ladder that
                        // climbed 34 ΔL*; the ladder spans 12 now, so a
                        // single colour clears its contrast floor at every
                        // depth (measured 7.2:1 on bare rail against 3.4:1 on
                        // the old brightest rung) and the control stops
                        // changing colour as a reader scrolls past it.
                        color: chevronHover.hovered ? AppTheme.text
                                                    : AppTheme.textSecondary
                    }
                    // THE PLATE. It was hover-only and 20px square centred
                    // on the glyph's BOX — so at rest there was nothing under
                    // the mark at all, and on hover a plate appeared that was
                    // wider than the gutter and offset from the ink by the
                    // font's side bearing. It is always drawn now, positioned
                    // from the same one place the ink is, and hover is a
                    // brightening rather than an appearance.
                    Rectangle {
                        objectName: "railSpaceExpandPlate"
                        x: root.chevronPlateLeft
                        width: root.chevronPlateWidth
                        height: root.chevronPlateHeight
                        // Centred on the ROW, not on the glyph's box — the
                        // glyph is the thing that moves to meet it.
                        anchors.verticalCenter: parent.verticalCenter
                        radius: AppTheme.radiusSm
                        // ONE RUNG UP THE RAIL'S OWN LADDER, not `hover`.
                        // A resting plate filled with `hover` measures L*
                        // 19.75 on the depth-1 region — above the ladder's
                        // deliberate ceiling (16.99) AND above the room list
                        // beside it (13.83) — so one slab per expandable
                        // Space would have rebuilt exactly what the ceiling
                        // round measured and removed: the rail as the
                        // brightest vertical band in the window. Stepping the
                        // ladder keeps the plate inside the system it sits
                        // in, and it follows every theme by construction.
                        // TWO rungs, not one: one rung is the same 3.4 ΔL*
                        // that separates two REGIONS, and a control has to
                        // read as a control rather than as another band. Two
                        // puts it 6.75 ΔL* over the region it sits on and
                        // still four below `hover`, which is what the hover
                        // state now has left to say.
                        color: chevronHover.hovered
                               ? AppTheme.hover
                               : AppTheme.railNestSurfaces[
                                   Math.min(
                                       AppTheme.railNestSurfaces.length - 1,
                                       spaceItem.innermostTint + 2)]
                        Behavior on color { ColorAnimation { duration: 90 } }
                        z: -1
                    }
                    HoverHandler { id: chevronHover }
                    TapHandler {
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: app.railLayout.toggleSpaceExpanded(
                                      spaceItem.spaceId)
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: spaceItem.expanded
                                     ? qsTr("Collapse space")
                                     : qsTr("Expand space")
                }

                Rectangle {
                    id: spaceTile
                    objectName: "railSpaceTile"
                    width: spaceItem.rowTileSize
                    height: spaceItem.rowTileSize
                    // CENTRED on the column, not left-aligned to it: a smaller
                    // tile inset on one side only reads as misaligned, where
                    // the same tile inset equally on both reads as smaller.
                    x: root.tileColumnX
                       + Math.round((root.railTileSize
                                     - spaceItem.rowTileSize) / 2)
                    y: spaceItem.contentTop + AppTheme.scaled(4)
                       + spaceItem.dragLift
                    // THIS ROW'S OWN SIZE, not the top-level one. A nested
                    // tile drawn at the full tile's radius is proportionally
                    // rounder than its parent, which reads as a different
                    // shape rather than as a smaller one.
                    radius: root.tileRadiusFor(spaceItem.rowTileSize)
                    // ACTIVE is ONE language for every tile in the rail: the
                    // accent ring above, plus a soft accent WASH here (a tint,
                    // not a block). A solid bolt fill four pixels inside a bolt
                    // ring reads as one yellow blob rather than as "you are
                    // here".
                    color: spaceItem.dropTarget ? AppTheme.accentSoft
                           : spaceItem.isActive ? AppTheme.accentSoft
                           : spaceItem.isFolder ? AppTheme.cardElevated
                           : spaceItem.pseudo ? AppTheme.cardElevated
                                              : "transparent"
                    // The GROUP target wears the rail's own "you are here"
                    // colour, thicker: a release here merges the two into a
                    // folder, and the accent is the one ink this column uses
                    // to say "this tile is the one that matters".
                    border.width: spaceItem.dropTarget ? 3 : 0
                    border.color: AppTheme.accent
                    // Full opacity, always. The tile keeps its normal image
                    // while it is dragged; dimming it made the one thing the
                    // user is looking at the hardest thing to see.
                    //
                    // Scale carries the merge: the dragged tile shrinks onto
                    // the target it has parked on (see dragLift), and the
                    // target opens up a little to receive it, so the two read
                    // as about to become one thing rather than as one tile
                    // sitting on another.
                    scale: spaceItem.dragged
                           ? (root.groupAnchorY >= 0 ? 0.56 : 1.06)
                           : spaceItem.dropTarget ? 1.08 : 1.0

                    Behavior on color { ColorAnimation { duration: 120 } }
                    Behavior on scale { NumberAnimation { duration: 90 } }

                    // Pseudo rows use monochrome icons; real Spaces show
                    // palette initials until the avatar loads.
                    Icon {
                        anchors.centerIn: parent
                        visible: spaceItem.pseudo
                        name: spaceItem.isHome ? "home"
                              : spaceItem.isPeople ? "person" : "workspaces"
                        // ONE RULE, HALF THE TILE. These were RAW literals
                        // — 22 and 20, unscaled, so they stayed put at 140%
                        // interface while the tile around them grew — and at
                        // a 59px tile they gave a glyph ratio of 0.37 against
                        // Material's and Discord's 0.50.
                        size: root.railChipIconSize
                        // Follows the tile: accent ink on the active wash, the
                        // plain icon ink otherwise. accentText was the ink for
                        // a solid fill that no longer exists, and on a soft
                        // wash it is unreadable.
                        // WHITE WHEN SELECTED, and it was the ACCENT on an
                        // accent wash: measured 1.41:1, against 5.95:1 for
                        // the unselected tile beside it. Selection made the
                        // one item you most need to read four times harder to
                        // read. The wash carries "you are here"; the glyph's
                        // job is to stay legible on it.
                        color: spaceItem.isActive ? AppTheme.text
                                                  : AppTheme.textSecondary
                    }

                    // The folder tile: a COMPOSITE of the Spaces inside it,
                    // the way Discord's is. A generic letter tile tells the
                    // user the one thing they already know ("this is a
                    // folder"); the member avatars tell them which folder,
                    // which is the only question a collapsed folder raises.
                    Loader {
                        anchors.fill: parent
                        active: spaceItem.isFolder
                        visible: active
                        sourceComponent: FolderTile {
                            members: spaceItem.memberPreview
                            fallbackName: spaceItem.name
                            colorKey: spaceItem.entryId
                            highlighted: spaceItem.dropTarget
                        }
                    }

                    Avatar {
                        anchors.fill: parent
                        visible: !spaceItem.pseudo && !spaceItem.isFolder
                        // MUST equal the rendered edge: Avatar bakes the
                        // rounded-square mask as `radius * 1000 / size`
                        // permille of the bitmap, so a stale 40 here gave a
                        // scaled tile a corner 40% too round.
                        // THIS ROW'S SIZE, not the base one. `Avatar.size` is
                        // the mask's permille denominator, so a nested tile
                        // rendered at 41 with a mask baked from 48 came out
                        // at r/size 0.356 against the 0.30 every other tier
                        // uses — MORE round than the band containing it,
                        // which is the classic wrong-nesting read. The
                        // Rectangle beneath it was already correct, which is
                        // what made the two disagree.
                        size: spaceItem.rowTileSize
                        circle: false
                        squareRadius: root.tileRadiusFor(spaceItem.rowTileSize)
                        labelSize: AppTheme.scaled(15)
                        name: spaceItem.name
                        colorKey: spaceItem.spaceId
                        mxc: spaceItem.avatarUrl
                    }

                    // Unread count badge (rail-coloured ring per design).
                    Rectangle {
                        visible: spaceItem.unreadTotal > 0
                                 && !spaceItem.isActive
                        width: Math.max(AppTheme.scaled(18),
                                        badgeLabel.implicitWidth
                                        + AppTheme.scaled(6))
                        height: AppTheme.scaled(18)
                        radius: height / 2
                        color: spaceItem.highlightTotal > 0
                               ? AppTheme.mentionBadge : AppTheme.unreadBadge
                        border.color: AppTheme.rail
                        border.width: 2
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.topMargin: -AppTheme.scaled(5)
                        anchors.rightMargin: -AppTheme.scaled(5)

                        Label {
                            id: badgeLabel
                            anchors.centerIn: parent
                            text: spaceItem.unreadTotal > 99
                                  ? "99+" : spaceItem.unreadTotal.toString()
                            font.pixelSize: AppTheme.textMicro
                            font.weight: AppTheme.weightBold
                            color: spaceItem.highlightTotal > 0
                                   ? AppTheme.dangerText : AppTheme.accentText
                        }
                    }
                }

                // Drag to rearrange. Vertical only — the rail is a column, and
                // a sideways twitch is not a reorder. Pseudo rows are excluded:
                // "All rooms" is a view of everything.
                //
                // The gesture's state lives in the MODEL, not here.
                //
                // ── BOUND TO THE TILE BAND, AND IT WAS NOT ─────────────────
                //
                // A handler acts within its PARENT, and this one's parent was
                // the whole delegate — which includes `expansionCol`, the
                // revealed rooms drawn underneath the tile. So a press on a
                // ROOM armed the SPACE's drag, and the room's own handler
                // never activated at all: instrumented, `onActiveChanged`
                // never fired once. What looked like a working room reorder
                // in a capture was the activity sort re-running because the
                // tap underneath had opened the room.
                //
                // Reparented to an item that covers the tile band only. Same
                // device `expandChevronArea` already uses two hundred lines
                // up, and the reason is the same: a handler's reach is its
                // parent's geometry, so the geometry is the API.
                Item {
                    id: tileDragArea
                    width: parent.width
                    height: spaceItem.tileBandHeight
                    y: spaceItem.contentTop
                DragHandler {
                    id: tileDrag
                    enabled: spaceItem.draggable
                    target: null
                    xAxis.enabled: false
                    onActiveChanged: {
                        if (active) {
                            root.beginTileDrag(spaceItem.entryId,
                                               centroid.scenePosition.y)
                        } else if (app.railEntries.draggingEntryId
                                   === spaceItem.entryId) {
                            app.railEntries.endDrag(true)
                        }
                    }
                    onCentroidChanged: {
                        if (active)
                            root.updateTileDrag(centroid.scenePosition.y)
                    }
                }
                }

                // Right-click: folders are renamed and unmade here, and a
                // Space can be filed without a drag. The primary way to MAKE
                // one is dropping a Space onto another Space.
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    enabled: !root.dragging
                    onTapped: (eventPoint) => {
                        railMenu.entryId = spaceItem.entryId
                        railMenu.isFolder = spaceItem.isFolder
                        railMenu.spaceId = spaceItem.spaceId
                        railMenu.inFolder = spaceItem.inFolder
                                            ? spaceItem.folderId : ""
                        railMenu.collapsed = spaceItem.collapsed
                        railMenu.hierarchyChild = spaceItem.hierarchyChild
                        railMenu.folderName = spaceItem.isFolder
                                              ? spaceItem.name : ""
                        railMenu.spaceMuted =
                            spaceItem.isRealSpace
                            && app.spaceIsMuted(spaceItem.spaceId)
                        railMenu.spaceUnread = spaceItem.unreadTotal
                        var p = spaceItem.mapToItem(Overlay.overlay,
                                                    eventPoint.position.x,
                                                    eventPoint.position.y)
                        railMenu.popup(Overlay.overlay, p.x, p.y)
                    }
                }

                HoverHandler { id: spaceHover }
                // SAME OUTSET AS THE RING, and it was a raw -3 against the
                // ring's 2. The chevron's gap is budgeted against the tile's
                // visible edge, so an outset that reaches further out than
                // the ring quietly spent that gap while a row was hovered —
                // the same "the visible edge is not `tileColumnX`" defect
                // this file fixed one property up, left behind as a literal.
                Rectangle {
                    anchors.fill: spaceTile
                    anchors.margins: -root.tileRingOutset
                    radius: spaceTile.radius + root.tileRingOutset
                    color: AppTheme.hover
                    visible: spaceHover.hovered && !spaceItem.isActive
                             && !root.dragging
                    // BELOW the tree, not level with it. At equal z the later
                    // sibling wins, so the halo painted over the last three
                    // pixels of the elbow and the line stopped on the halo
                    // instead of on the tile — only while hovered, which is
                    // exactly when the reader is looking at that row.
                    z: -2
                }

                TapHandler {
                    // A single tap on a REAL Space opens its overview — the
                    // unified rooms-and-spaces list REPLACES the chat view
                    // (openSpaceHome also activates the space, so the
                    // room-list column follows). The pseudo tiles only filter:
                    // they have no overview to open and must not tear down the
                    // open room. There is deliberately NO double-tap; the
                    // chevron is the one expansion trigger. Scoped to the tile
                    // band (the expansion rows below carry their own handlers)
                    // and excluding the chevron's gutter — TapHandlers are
                    // non-exclusive across subtrees, so without the exclusion a
                    // chevron click would also navigate.
                    enabled: !root.dragging
                    function pointOnChevron(eventPoint) {
                        if (!expandChevronArea.visible)
                            return false
                        var cp = spaceItem.mapToItem(expandChevronArea,
                                                     eventPoint.position.x,
                                                     eventPoint.position.y)
                        return cp.x >= 0 && cp.x <= expandChevronArea.width
                               && cp.y >= 0
                               && cp.y <= expandChevronArea.height
                    }
                    onTapped: (eventPoint) => {
                        if (eventPoint.position.y > spaceItem.tileBandHeight)
                            return
                        if (pointOnChevron(eventPoint))
                            return
                        if (spaceItem.isFolder) {
                            app.railLayout.setFolderCollapsed(
                                spaceItem.entryId, !spaceItem.collapsed)
                            return
                        }
                        if (spaceItem.isRealSpace)
                            app.openSpaceHome(spaceItem.spaceId)
                        else if (app.spaces)
                            app.spaces.activeSpaceId = spaceItem.spaceId
                    }
                }

                // ── The tooltip hangs off the rail, not over it ─────────
                //
                // Attached, not a declared child: a declared ToolTip is a full
                // Popup (background + Label) instantiated PER ROW, and the
                // attached form reuses the one shared instance Main.qml
                // hardens to plain text. A Space name is remote text, so that
                // hardening is not optional and a per-row ToolTip must not be
                // introduced to move this.
                //
                // Qt centres an attached tooltip on the item it is attached to
                // and puts it ABOVE. Attached to the row, that is the middle
                // of the rail one row up — measured covering 15 of the 28
                // pixels of the tile above, in a column whose only identity
                // cue is a two-letter avatar, while ~37px of rail sat unused
                // beside it. So it is attached to an invisible anchor that
                // starts at the rail's right edge and hangs BELOW the row:
                // centred there the tooltip clears the rail entirely, and
                // "above the anchor" puts it beside the row it describes
                // rather than over the one before it.
                Item {
                    objectName: "railSpaceTipAnchor"
                    x: spaceItem.width
                    y: spaceItem.contentTop + spaceItem.tileBandHeight
                    width: AppTheme.scaled(150)
                    height: 1
                    ToolTip.visible: spaceHover.hovered && !root.dragging
                    ToolTip.text: spaceItem.Accessible.name
                    ToolTip.delay: 500
                }

                // THE REVEALED ROOMS NEED NO FIELD OF THEIR OWN.
                //
                // They used to get a separate rectangle, because a nested
                // Space run sat on a tint while a room run floated on bare
                // rail though both are the same statement about the same
                // tile. Once a tile draws the region it OWNS, that rectangle
                // is the owner's own layer: it already spans this delegate,
                // tile band and revealed rooms together, at the inset and
                // tint one step in from this row's own. Two things drawing
                // one region is how they drift apart.

                // Inline expansion: up to `revealed` of the space's top rooms
                // as 28px tiles, then a "+N" pill revealing 5 more. Tiles
                // indent one step past the owning tile so the hierarchy reads.
                // The reorder gesture, on an item of its OWN. See the long
                // note below for why it is neither on each row nor on the
                // Column itself.
                Item {
                    id: roomDragArea
                    x: 0
                    y: expansionCol.y
                    width: parent.width
                    height: expansionCol.height
                    visible: expansionCol.visible
                    z: 1
                    function roomIndexAt(py) {
                        var pitch = root.railRoomRowBand + expansionCol.spacing
                        return Math.floor(Math.max(0, py) / pitch)
                    }
                    DragHandler {
                        target: null
                        xAxis.enabled: false
                        onActiveChanged: {
                            if (active) {
                                spaceItem.roomDragIndex =
                                    roomDragArea.roomIndexAt(
                                        centroid.pressPosition.y)
                                spaceItem.roomPreview =
                                    spaceItem.revealedRooms.slice(
                                        0, spaceItem.revealed)
                            } else {
                                spaceItem.commitRoomOrder()
                            }
                        }
                        onCentroidChanged: {
                            if (active) {
                                spaceItem.moveRoomPreview(
                                    roomDragArea.roomIndexAt(
                                        centroid.position.y))
                            }
                        }
                    }
                }

                Column {
                    id: expansionCol
                    // ── ITS OWN GEOMETRY, AND IT LOST IT ────────────────
                    //
                    // `visible`, `y`, `width` and `spacing` were deleted by a
                    // careless edit that removed a sibling block and took the
                    // four lines above it. ONE mistake, three live failures,
                    // and every one of them looked like a different bug:
                    //
                    //  * no Space revealed any room anywhere, because a
                    //    Column with no width lays out nothing;
                    //  * `visible` defaulted to TRUE, so the delegate added
                    //    `expansionCol.height + 2` to EVERY row while
                    //    `rowBand()` — which the drag's pointer arithmetic
                    //    accumulates — did not, drifting ~2px per row;
                    //  * so a drop landed a row and a half from the pointer
                    //    and silently made a folder out of a Space the user
                    //    was never pointing at.
                    //
                    // A full CTest run passed throughout: the mock fixture
                    // reveals no rooms, so the Column is empty there and the
                    // divergence is zero. GENERALISE: when a slice-and-splice
                    // edit removes a block, diff what it actually removed —
                    // the boundary you searched for is not the boundary you
                    // meant.
                    visible: spaceItem.revealed > 0
                             && spaceItem.revealedRooms.length > 0
                             && !root.dragging
                    y: spaceItem.contentTop + spaceItem.tileBandHeight
                    width: parent.width
                    spacing: 2

                    Repeater {
                        id: roomRepeater
                        model: !expansionCol.visible
                               ? []
                               : (spaceItem.roomPreview
                                  ? spaceItem.roomPreview
                                  : spaceItem.revealedRooms.slice(
                                        0, spaceItem.revealed))
                        delegate: Item {
                            id: expansionRoomRow
                            required property var modelData
                            required property int index
                            width: expansionCol.width
                            height: root.railRoomRowBand
                            Rectangle {
                                anchors.fill: roomTile
                                anchors.margins: -root.tileRingOutset
                                // CONCENTRIC, and derived: `radius: 10` was
                                // right only at 100% scale and only at the
                                // minimum rail width. A rounded rect outset
                                // by N is concentric only at its tile's
                                // radius PLUS N, at every size.
                                radius: roomTile.radius + root.tileRingOutset
                                color: AppTheme.hover
                                visible: roomHover.hovered
                            }
                            Rectangle {
                                id: roomTile
                                objectName: "railRevealedRoomTile"
                                width: root.railRoomTileSize
                                height: root.railRoomTileSize
                                radius: root.tileRadiusFor(
                                            root.railRoomTileSize)
                                color: "transparent"
                                // THE SAME x AS EVERY OTHER TILE. A room used
                                // to sit half a step further in, giving the
                                // expansion column an offset that lined up
                                // with nothing; now its 28px size is what
                                // says it is a room, exactly as Element's own
                                // one-step avatar shrink does, and the column
                                // stays a column.
                                x: root.tileColumnX
                                   + Math.round((root.railTileSize
                                                 - root.railRoomTileSize) / 2)
                                anchors.verticalCenter: parent.verticalCenter
                                Avatar {
                                    anchors.fill: parent
                                    size: root.railRoomTileSize
                                    circle: expansionRoomRow.modelData
                                                .isDirect === true
                                    squareRadius: root.tileRadiusFor(
                                                      root.railRoomTileSize)
                                    labelSize: AppTheme.scaled(11)
                                    name: expansionRoomRow.modelData.name || ""
                                    colorKey: expansionRoomRow.modelData
                                                  .identityColorKey
                                              || expansionRoomRow.modelData
                                                     .roomId
                                    mxc: expansionRoomRow.modelData
                                             .avatarUrl || ""
                                }
                                Rectangle {
                                    visible: expansionRoomRow.modelData
                                                 .hasUnread === true
                                    width: AppTheme.scaled(10)
                                    height: AppTheme.scaled(10)
                                    radius: height / 2
                                    color: (expansionRoomRow.modelData
                                                .highlightCount || 0) > 0
                                           ? AppTheme.mentionBadge
                                           : AppTheme.unreadBadge
                                    border.color: AppTheme.rail
                                    border.width: 2
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.topMargin: -2
                                    anchors.rightMargin: -2
                                }
                            }
                            HoverHandler { id: roomHover }
                            TapHandler {
                                // Opening from the rail also activates the
                                // space so the room-list column follows —
                                // openRoom itself never touches activeSpaceId.
                                onTapped: {
                                    if (app.spaces)
                                        app.spaces.activeSpaceId =
                                            spaceItem.spaceId
                                    app.openRoom(
                                        expansionRoomRow.modelData.roomId)
                                }
                            }
                            // Off the rail, for the reason the Space tile's
                            // own anchor carries: centred on a 28px tile a
                            // tooltip covers its neighbours, and the rail has
                            // nothing else to identify them by.
                            Item {
                                x: expansionRoomRow.width
                                   - expansionRoomRow.x
                                y: expansionRoomRow.height
                                width: AppTheme.scaled(150)
                                height: 1
                                ToolTip.visible: roomHover.hovered
                                ToolTip.text: expansionRoomRow.modelData.name
                                              || expansionRoomRow.modelData
                                                     .roomId
                                ToolTip.delay: 300
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: expansionRoomRow.modelData.name
                                             || expansionRoomRow.modelData
                                                    .roomId
                        }
                    }

                    Item {
                        visible: spaceItem.revealedRooms.length
                                 > spaceItem.revealed
                        width: expansionCol.width
                        height: AppTheme.scaled(22)
                        Rectangle {
                            id: morePill
                            objectName: "railSpaceMoreButton"
                            width: AppTheme.scaled(32)
                            height: AppTheme.scaled(18)
                            radius: height / 2
                            color: moreHover.hovered ? AppTheme.hover
                                                     : AppTheme.cardElevated
                            border.color: AppTheme.border
                            border.width: 1
                            x: root.tileColumnX
                               + Math.round((root.railTileSize - morePill.width) / 2)
                            anchors.verticalCenter: parent.verticalCenter
                            Label {
                                anchors.centerIn: parent
                                text: "+" + Math.min(
                                          5,
                                          spaceItem.revealedRooms.length
                                          - spaceItem.revealed)
                                font.pixelSize: AppTheme.textMicro
                                font.weight: AppTheme.weightBold
                                color: AppTheme.textSecondary
                            }
                        }
                        HoverHandler { id: moreHover }
                        TapHandler {
                            onTapped: root.showMoreRooms(spaceItem.spaceId)
                        }
                        Item {
                            x: morePill.width
                            y: morePill.height
                            width: AppTheme.scaled(150)
                            height: 1
                            ToolTip.visible: moreHover.hovered
                            ToolTip.text: qsTr("Show more rooms")
                            ToolTip.delay: 300
                        }
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Show more rooms")
                    }
                }
            }

            // Add Space: part of the list CONTENT, so it sits directly below
            // the last Space tile, scrolls with the tiles, and never overlaps
            // the pinned Settings/account cluster.
            footer: Item {
                width: list.width
                // THE TILE'S OWN HEIGHT, and it was a literal 48 that
                // predates a resizable tile. With the tile at 59 the content
                // reached 63 inside a 48px footer, so `contentHeight` was 15
                // short: the "+" clipped at the bottom of a scrolled rail and
                // the scrollbar's range was wrong.
                height: railAddSpaceButton.visible
                        ? root.railTileSize + 2 * root.rowPad : 0
                IconButton {
                    id: railAddSpaceButton
                    objectName: "railAddSpaceButton"
                    y: AppTheme.scaled(4)
                    x: root.tileColumnX
                    implicitWidth: root.railTileSize
                    implicitHeight: root.railTileSize
                    radius: root.railTileRadius
                    iconName: "add"
                    iconSize: root.railChipIconSize
                    visible: app.loggedIn && app.conversations
                             && app.conversations.supported
                    Accessible.name: qsTr("Create a Space")
                    ToolTip.text: qsTr("Create a Space")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.createSpaceRequested()
                    // A QUIET SIBLING, not a hole. An outlined transparent
                    // square in a column of filled tiles reads as a GAP —
                    // and it sits at the column's end, where the rail was
                    // already dissolving. Filled at 40% so it is clearly the
                    // lightest tile without being an absence.
                    Rectangle {
                        anchors.fill: parent
                        z: -1
                        radius: root.railTileRadius
                        color: Qt.rgba(AppTheme.cardElevated.r,
                                       AppTheme.cardElevated.g,
                                       AppTheme.cardElevated.b, 0.4)
                        border.width: 1
                        border.color: AppTheme.borderStrong
                    }
                }
            }
        }

        // ── Bottom cluster: settings + account ─────────────────────────────
        // THE SAME DIVIDER THE HANDOFF ROW USES, centred on the TILE COLUMN.
        // This one was `spacing12` against the RAIL while every tile aligns
        // to the column — so it published one centre line and the tiles
        // another, 8.8px apart, and it was the separator that was right. It
        // is also why the cog and avatar beside it read as a separate widget
        // bolted under the rail rather than as the bottom of the same column.
        Rectangle {
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.railDividerX
            implicitWidth: root.railDividerWidth
            implicitHeight: 2
            radius: 1
            color: AppTheme.separator
            visible: app.loggedIn
        }

        // ONE GAP, the column's own. This was `spacing8` where every tile
        // above it is separated by 12.
        Item { implicitHeight: root.railTileGap; visible: app.loggedIn }

        IconButton {
            id: railSettingsButton
            objectName: "railSettingsButton"
            // ON THE COLUMN, which is now the rail's own centre. The note
            // that used to sit here said the tiles above were not centred, so
            // a centred cog would be the only thing that moved — true while
            // the margins were asymmetric, and false since.
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.tileColumnX
            implicitWidth: root.railTileSize
            implicitHeight: root.railTileSize
            radius: root.railTileRadius
            // A FILL, like the Home and People chips. A 17x19 glyph in a
            // 59px invisible box sat directly above a 59px solid disc — an
            // optical weight ratio near 5:1, which is why the two could not
            // read as siblings.
            restingColor: AppTheme.cardElevated
            iconName: "settings"
            iconSize: root.railChipIconSize
            // Accent chip while the in-shell Settings view is open;
            // clicking again returns to chat.
            active: app.currentScreen === 2
            visible: app.loggedIn
            // v0.7.x: the badge names WHY the cog wants attention, so a
            // screen reader is not left with a bare "Settings" while a red
            // dot sits on it.
            // ONE badge, never two dots on 68px of chrome. Verification
            // outranks an update: a security state the user must act on is
            // not the same class of thing as a release being available, and
            // colouring them alike would devalue the red one.
            // The PERSISTENT fact, not the dismissible card: dismissing the
            // corner prompt stops the interruption, and the badge is what is
            // left to say an update is still waiting.
            readonly property bool _updateBadge:
                app.updateManager && app.updateManager.updateAvailable
            readonly property string _attentionText:
                app.sessionVerificationWarning
                    ? qsTr("Settings — this session is not verified")
                    : (_updateBadge
                       ? qsTr("Settings — a Lightning update is available")
                       : qsTr("Settings"))
            Accessible.name: _attentionText
            ToolTip.text: _attentionText
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: app.currentScreen === 2 ? app.showMain()
                                               : app.showSettings()

            // Attention badge: this session is not verified AND the user
            // has not dismissed the reminder. A dot, not a full "!" glyph —
            // the rail is 68px of chrome and the tooltip carries the words.
            // Ringed in the rail colour so it reads as a badge sitting ON
            // the cog rather than part of the glyph.
            Rectangle {
                objectName: "railSettingsAlertBadge"
                visible: app.sessionVerificationWarning || parent._updateBadge
                anchors.right: parent.right
                anchors.top: parent.top
                // ON THE GEAR'S SHOULDER, not floating in the button's
                // padding. It was anchored to the 59px BUTTON while the glyph
                // inside it is 23 — measured at 24.9px from the glyph's
                // centre, further than the glyph is wide, so it read as an
                // unattached red dot up and to the right of the cog.
                anchors.margins:
                    Math.round((root.railTileSize - root.railChipIconSize) / 2)
                    - AppTheme.scaled(3)
                width: AppTheme.scaled(10)
                height: AppTheme.scaled(10)
                radius: height / 2
                // `danger`/`warning` became INK-ONLY roles on 2026-08-21
                // (they route light on dark themes so they stay AA as text);
                // a badge is a FILL and must ask for the saturated fill by
                // name, or the dot renders as a pale rose smudge on the rail.
                color: app.sessionVerificationWarning ? AppTheme.dangerFill
                                                      : AppTheme.warningFill
                border.color: AppTheme.rail
                border.width: 2
            }
        }

        Item { implicitHeight: root.railTileGap; visible: app.loggedIn }

        // Account avatar (40 px circle) with presence dot; opens the
        // account switcher popover.
        Item {
            id: railAccount
            objectName: "railAccountTile"
            // ON THE COLUMN — see the cog above for why the note that used
            // to be here is no longer true.
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.tileColumnX
            // implicitWidth, NOT width: this is a ColumnLayout child, and a
            // layout takes an aligned item's PREFERRED size — which falls
            // back to whatever `width` happened to be at the first pass and
            // then never looks again. Written as `width` it stayed 40px at
            // every interface size while the cog above it (already an
            // implicit size) scaled correctly.
            implicitWidth: root.railTileSize
            implicitHeight: root.railTileSize
            visible: app.loggedIn

            // Invokable results do not re-evaluate on signals; refresh the
            // record whenever the registry or selection changes.
            property var activeAccount: ({})
            function refreshAccount() {
                activeAccount = app.accounts
                    ? app.accounts.account(app.accounts.activeUserId) : ({})
            }
            Component.onCompleted: refreshAccount()
            Connections {
                target: app.accounts
                function onAccountsChanged() { railAccount.refreshAccount() }
                function onActiveUserIdChanged() { railAccount.refreshAccount() }
            }
            readonly property string localpart: {
                var uid = app.accounts ? (app.accounts.activeUserId || "") : ""
                if (uid.startsWith("@")) uid = uid.slice(1)
                var colon = uid.indexOf(":")
                return colon > 0 ? uid.slice(0, colon) : uid
            }

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Account menu for %1")
                             .arg(app.accounts ? app.accounts.activeUserId : "")

            Avatar {
                id: railAvatar
                anchors.fill: parent
                // Same rule as the Space tiles: Avatar's mask is a permille
                // of `size`, so it must be the rendered edge.
                size: root.railTileSize
                circle: true
                name: railAccount.activeAccount.displayName
                      || railAccount.localpart
                mxc: railAccount.activeAccount.avatarUrl || ""
                // Key by the MXID like every other self-avatar surface —
                // a display-name key gave the rail its own colour and
                // recoloured on rename.
                colorKey: app.accounts ? app.accounts.activeUserId : ""
            }
            // THIS BADGE SHOWS THE ACCOUNT'S REAL PRESENCE.
            //
            // It was a CONNECTION indicator — green when the sync socket was
            // up — and it was the only self-status in the app, so the answer
            // to "what am I showing as?" was a question it could not answer.
            // Reported 2026-09-19 as not showing the correct status.
            //
            // The lifecycle (watch/unwatch, unknown renders NOTHING rather
            // than a fabricated Offline, offline drawn as a hollow ring so
            // the three states differ in FORM and not only in hue) all lives
            // in PresenceDot, which every other surface already uses. The
            // rail was the one place hand-rolling a rival indicator.
            //
            // CONNECTION IS NOT LOST, it moves to the tooltip. That is the
            // same argument PresenceDot itself makes about `unavailable`: a
            // dot has no room for prose, so the only thing it could do with a
            // second fact is paint another colour, which is a fabricated
            // indicator by another name. The sentence goes where there is
            // room for a sentence.
            PresenceDot {
                id: railSelfPresence
                objectName: "railSelfPresenceDot"
                userId: app.accounts ? (app.accounts.activeUserId || "") : ""
                ring: AppTheme.rail
                dotSize: root.selfDotSize
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: root.selfDotMargin
                anchors.bottomMargin: root.selfDotMargin
                Accessible.role: Accessible.Indicator
                Accessible.name: statusText
            }
            // AND WHEN THE SERVER HAS NO PRESENCE TO GIVE, the badge still
            // has to say something: PresenceDot renders nothing for an
            // unknown state, which is right for a PEER and would leave the
            // user's own tile with no indicator at all on a server with
            // presence disabled. This is the old connection dot, kept for
            // exactly that case and visible only then.
            Rectangle {
                objectName: "railConnectionDot"
                visible: !railSelfPresence.visible
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: root.selfDotMargin
                anchors.bottomMargin: root.selfDotMargin
                width: root.selfDotSize
                height: root.selfDotSize
                radius: height / 2
                border.color: AppTheme.rail
                border.width: root.selfDotRing
                color: app.connectionStatus === qsTr("Connected")
                       ? AppTheme.presenceOnline : AppTheme.presenceAway
                Accessible.role: Accessible.Indicator
                Accessible.name: app.connectionStatus === qsTr("Connected")
                                 ? qsTr("Connected to your homeserver")
                                 : qsTr("Not connected to your homeserver")
            }

            HoverHandler { id: accountHover }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: AppTheme.radiusPill
                color: AppTheme.hover
                visible: accountHover.hovered
                z: -1
            }
            TapHandler { onTapped: railAccountMenu.open() }
            ToolTip.visible: accountHover.hovered
            ToolTip.text: app.accounts ? (app.accounts.activeUserId || "") : ""
            ToolTip.delay: 500

            AccountMenu {
                id: railAccountMenu
                x: parent.width + AppTheme.spacing8
                // v0.6.5: the vertical identity-card stack can grow far
                // taller than the old single-header popover. By default it
                // still grows upward from the rail avatar's bottom (unchanged
                // behavior), but it must never push its top above the
                // window's top edge. `root.height` is read only to force
                // this binding to re-evaluate on a window resize —
                // Item.mapFromItem() results are not tracked reactively by
                // QML's binding engine on their own.
                readonly property real _windowTopLocalY: {
                    var _dep = root.height
                    return parent ? parent.mapFromItem(null, 0, 0).y : 0
                }
                y: Math.max(_windowTopLocalY + AppTheme.spacing12,
                            parent.height - implicitHeight)
            }
            // Development-only: the screenshot-demo "account-switching" scenario
            // opens the real account switcher popover. Null target in a
            // non-demo build makes this an inert no-op.
            Connections {
                target: app.demo
                enabled: app.screenshotDemoActive
                function onAccountSwitcherRequested() { railAccountMenu.open() }
            }
        }
    }

    // ── Folders ──────────────────────────────────────────────────────────
    // Ordering and grouping the rail is DEVICE-LOCAL, deliberately: Matrix
    // has no standard for either, so anything stored on the server would be a
    // private invention only Lightning could read. See RailLayoutStore.
    AppMenu {
        id: railMenu
        objectName: "railContextMenu"
        property string entryId: ""
        property string spaceId: ""
        property string inFolder: ""
        property string folderName: ""
        property bool isFolder: false
        property bool collapsed: false
        // A subspace is Matrix's arrangement. It can be expanded and opened;
        // it cannot be filed or ordered, and offering to would promise
        // something this layer cannot deliver.
        property bool hierarchyChild: false
        // A REAL Space (not Home, not "Other rooms", not a local folder), so
        // the Space-only actions are offered only where they mean something.
        readonly property bool isRealSpace:
            !isFolder && spaceId.charAt(0) === "!"
        // Sampled when the menu opens, not bound: spaceIsMuted() is a call
        // over every room in the Space and carries no NOTIFY of its own.
        property bool spaceMuted: false
        // Same treatment, same reason: read once from the row that was
        // right-clicked, so "Mark as read" can be honestly disabled when
        // there is nothing to mark.
        property int spaceUnread: 0
        // Sable's menu names the Space it belongs to at the top. AppMenu's
        // own context header does that here — the row it was opened from is
        // no longer under the pointer once the menu is up.
        contextLabel: railMenu.isFolder
                      ? railMenu.folderName
                      : (app.spaces ? app.spaces.spaceName(railMenu.spaceId)
                                    : "")

        AppMenuItem {
            iconName: railMenu.collapsed ? "expand_more" : "expand_less"
            text: railMenu.collapsed ? qsTr("Expand folder")
                                     : qsTr("Collapse folder")
            visible: railMenu.isFolder
            onTriggered: app.railLayout.setFolderCollapsed(railMenu.entryId,
                                                           !railMenu.collapsed)
        }
        AppMenuItem {
            iconName: "edit_square"
            text: qsTr("Rename folder…")
            visible: railMenu.isFolder
            onTriggered: {
                folderNameDialog.folderId = railMenu.entryId
                folderNameDialog.openFor(railMenu.folderName)
            }
        }
        AppMenuItem {
            objectName: "railDeleteFolder"
            iconName: "delete"
            text: qsTr("Delete folder")
            visible: railMenu.isFolder
            // The Spaces inside come back to the top level where the folder
            // was; nothing is left and nothing is removed from the account,
            // so this needs no confirmation.
            onTriggered: app.railLayout.deleteFolder(railMenu.entryId)
        }

        AppMenuSeparator { visible: railMenu.isFolder }

        // Mute the whole Space. Matrix has no "mute a Space" primitive — a
        // Space is a room with no timeline, so muting it silences nothing —
        // so this does what a person would otherwise do by hand to each room
        // inside it. Unmute restores "follow the account default", the state
        // a room is in before anyone touched it, rather than asserting "all
        // messages" for rooms that never asked for it.
        // ── Home: mark EVERY room read ───────────────────────────────
        //
        // Per-room and per-Space have existed for a while and the sweep did
        // not, so an account that had drifted could only be caught up one
        // room at a time. It lives on the Home tile because Home is the one
        // that means "everything"; the Space tile keeps its own scoped
        // version directly below.
        //
        // It clears the bell too: the receipt this sends is what
        // ActivityModel::markRoomReadUpTo listens for, so the room list and
        // the Activity badge come down together rather than one of them
        // being left behind.
        AppMenuItem {
            objectName: "railMarkAllRoomsRead"
            iconName: "done_all"
            text: qsTr("Mark all rooms read")
            visible: !railMenu.isFolder && railMenu.spaceId === ""
            // Nothing unread is nothing to do, and a control that would do
            // nothing should say so — the same restraint the Space item
            // below applies.
            enabled: railMenu.spaceUnread > 0
            onTriggered: app.roomList.markAllRoomsRead()
        }
        AppMenuItem {
            objectName: "railMarkSpaceRead"
            iconName: "done_all"
            text: qsTr("Mark as read")
            visible: railMenu.isRealSpace
            // A Space with nothing unread has nothing to mark, and a control
            // that would do nothing should say so rather than pretend.
            enabled: railMenu.spaceUnread > 0
            onTriggered: app.markSpaceRead(railMenu.spaceId)
        }
        AppMenuItem {
            objectName: "railMuteSpace"
            iconName: railMenu.spaceMuted ? "notifications" : "notifications_off"
            text: railMenu.spaceMuted ? qsTr("Unmute space")
                                      : qsTr("Mute space")
            visible: railMenu.isRealSpace
            onTriggered: app.setSpaceMuted(railMenu.spaceId,
                                           !railMenu.spaceMuted)
        }
        AppMenuItem {
            objectName: "railSpaceInvite"
            iconName: "person_add"
            text: qsTr("Invite")
            // Deliberately NOT gated on canInvite. That gate reads
            // app.roomInfo, which follows whatever surface last pointed it
            // somewhere — usually the open room, not this Space — so gating on
            // it would grey the row out because nobody has LOOKED, which is a
            // different and worse lie than offering something the server may
            // refuse. The invite dialog reports the server's answer honestly.
            visible: railMenu.isRealSpace
            onTriggered: spaceInviteDialog.openFor(railMenu.spaceId)
        }
        AppMenuItem {
            objectName: "railSpaceCopyLink"
            iconName: "content_copy"
            text: qsTr("Copy link")
            visible: railMenu.isRealSpace
            onTriggered: root.copySpaceLink(railMenu.spaceId)
        }
        AppMenuItem {
            objectName: "railSpaceShareLink"
            // Every glyph here has to exist in the bundled Material Symbols
            // SUBSET, which carries exactly the icons the app already uses —
            // a name the subset does not cover renders as tofu, not as a
            // missing icon. IconChromeTest pins the whole set.
            iconName: "link"
            text: qsTr("Share link…")
            visible: railMenu.isRealSpace
            onTriggered: shareLinkDialog.openFor(
                             railMenu.spaceId,
                             app.spaces.spaceName(railMenu.spaceId))
        }
        AppMenuItem {
            objectName: "railSpaceSettings"
            iconName: "settings"
            text: qsTr("Space settings")
            visible: railMenu.isRealSpace
            onTriggered: spaceSettings.openFor(railMenu.spaceId)
        }

        AppMenuSeparator { visible: railMenu.isRealSpace }

        AppMenuItem {
            objectName: "railNewFolder"
            iconName: "add"
            text: qsTr("New folder…")
            visible: !railMenu.hierarchyChild
            onTriggered: {
                folderNameDialog.folderId = ""
                folderNameDialog.pendingSpaceId =
                    railMenu.isFolder ? "" : railMenu.spaceId
                folderNameDialog.openFor("")
            }
        }
        AppMenuItem {
            iconName: "logout"
            text: qsTr("Move out of folder")
            visible: !railMenu.isFolder && railMenu.inFolder.length > 0
            onTriggered: app.railLayout.setSpaceFolder(railMenu.spaceId, "")
        }
        // One entry per existing folder, so filing a Space never requires a
        // drag — a rail with twenty Spaces is a long way to drag one.
        Repeater {
            model: (railMenu.isFolder || railMenu.hierarchyChild)
                   ? [] : app.railLayout.folders
            delegate: AppMenuItem {
                required property var modelData
                iconName: "workspaces"
                text: qsTr("Move to “%1”").arg(modelData.name || "")
                visible: modelData.id !== railMenu.inFolder
                onTriggered: app.railLayout.setSpaceFolder(railMenu.spaceId,
                                                           modelData.id)
            }
        }
    }

    // ── What the Space menu opens ────────────────────────────────────────
    // Hosted here, exactly as the folder-name dialog is: a delegate that
    // reached up into its host by id is how the reader popover's click ended
    // up silently dead, so the rows stay signal-only and every shared surface
    // is ONE instance owned by the view.

    // The clipboard write. A hidden TextEdit is the app's existing proxy for
    // this (RoomsPanel uses the same one for room links); text is cleared
    // immediately after the copy so a permalink does not sit in a live item.
    TextEdit {
        id: railLinkClipboard
        visible: false
        width: 0
        height: 0
    }

    // matrix.to, the public link — never an authenticated media or client URL.
    // Alias-preferred, id fallback, which is RoomListModel's own convention.
    function spaceLink(spaceId) {
        if (!app.roomList || !spaceId || spaceId.length === 0)
            return ""
        var row = app.roomList.findRoom(spaceId)
        return app.roomList.roomPermalink(
            spaceId, (row && row.canonicalAlias) || "")
    }
    function copySpaceLink(spaceId) {
        var link = root.spaceLink(spaceId)
        if (link.length === 0)
            return
        railLinkClipboard.text = link
        railLinkClipboard.selectAll()
        railLinkClipboard.copy()
        railLinkClipboard.text = ""
    }

    InvitePeopleDialog {
        id: spaceInviteDialog
        parent: Overlay.overlay
    }

    SpaceSettingsDialog {
        id: spaceSettings
        onInviteRequested: (spaceId) => spaceInviteDialog.openFor(spaceId)
    }

    // Share: the link itself, selectable, with the two things anyone actually
    // does with it. Lightning has no OS share sheet, and inventing one that
    // silently copied would make "Share" and "Copy link" the same control
    // under two names.
    AppDialog {
        id: shareLinkDialog
        objectName: "railShareLinkDialog"
        property string targetId: ""
        property string targetName: ""
        readonly property string link: root.spaceLink(targetId)
        title: qsTr("Share space")
        standardButtons: Dialog.Close
        parent: Overlay.overlay
        anchors.centerIn: parent

        function openFor(spaceId, name) {
            targetId = spaceId
            targetName = name || ""
            open()
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8
            Label {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                Layout.fillWidth: true
                Layout.maximumWidth: 380
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: shareLinkDialog.link.length > 0
                      ? qsTr("Anyone with this link can find “%1”. Whether "
                             + "they can join still depends on the space's "
                             + "access setting.").arg(
                            shareLinkDialog.targetName)
                      : qsTr("This space has no shareable address yet.")
            }
            AppTextField {
                id: shareLinkField
                objectName: "railShareLinkField"
                storm: true
                Layout.fillWidth: true
                Layout.minimumWidth: 340
                readOnly: true
                text: shareLinkDialog.link
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppButton {
                    storm: true
                    kind: "primary"
                    iconName: "content_copy"
                    text: qsTr("Copy link")
                    enabled: shareLinkDialog.link.length > 0
                    onClicked: root.copySpaceLink(shareLinkDialog.targetId)
                }
                AppButton {
                    storm: true
                    iconName: "explore"
                    text: qsTr("Open in browser")
                    enabled: shareLinkDialog.link.length > 0
                    onClicked: Qt.openUrlExternally(shareLinkDialog.link)
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    AppDialog {
        id: folderNameDialog
        objectName: "railFolderNameDialog"
        property string folderId: ""
        // Set when the folder is being created FROM a Space's menu: that
        // Space goes straight into it, which is what "new folder" means when
        // you asked for it while pointing at something.
        property string pendingSpaceId: ""
        title: folderId.length > 0 ? qsTr("Rename folder") : qsTr("New folder")
        standardButtons: Dialog.Ok | Dialog.Cancel
        parent: Overlay.overlay
        anchors.centerIn: parent

        function openFor(name) {
            folderNameField.text = name
            open()
            folderNameField.forceActiveFocus()
            folderNameField.selectAll()
        }

        onAccepted: {
            var name = folderNameField.text
            if (folderId.length > 0) {
                app.railLayout.renameFolder(folderId, name)
            } else {
                var created = app.railLayout.createFolder(name)
                if (created.length > 0 && pendingSpaceId.length > 0)
                    app.railLayout.setSpaceFolder(pendingSpaceId, created)
            }
            pendingSpaceId = ""
        }
        onRejected: pendingSpaceId = ""

        // A LAID-OUT content item, not a bare child. A raw Item dropped into
        // a Dialog's contentData is positioned by nothing: it sat at the
        // bottom of the panel, off-centre, with the header's space above it —
        // reported as "the text to create a folder is not centered and sitting
        // on the bottom of the bubble". AppDialog's own usage note says
        // ColumnLayout for exactly this reason.
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8

            Label {
                Layout.fillWidth: true
                Layout.maximumWidth: 300
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                // Says the one thing a person needs to know before naming it:
                // this is theirs, and nobody else will ever see it.
                text: qsTr("Folders are only on this device. Other clients, "
                           + "and everyone else, see your spaces unchanged.")
            }
            AppTextField {
                id: folderNameField
                objectName: "railFolderNameField"
                storm: true
                Layout.fillWidth: true
                Layout.minimumWidth: 260
                maximumLength: 40
                placeholderText: qsTr("Folder name")
                onAccepted: folderNameDialog.accept()
            }
        }
    }
}
