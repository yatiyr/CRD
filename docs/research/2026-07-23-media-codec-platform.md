# Research — 2026-07-23 — the OWNED MEDIA-CODEC PLATFORM (images · audio · video · transcode)

> The plan for handling EVERY media resource ourselves (user-directed: "videos, sound files, image files… webp avif gif
> tiff… video codecs, lossless and lossy sound formats… convert one into another"). Produces the **MED band** (D-007).
> Ground rules: zero 3rd-party in the product path (test oracles allowed); patent-encumbered formats are EXCLUDED BY
> NAME, never silently; every codec lands with hermetic analytic gates (the JPEG/PNG bar: fixtures with closed-form
> expectations); sizes are stated honestly — some of these are WEEKS each, and that is accepted ("if necessary we spend
> months").

## Question

What does a complete, owned media platform look like for an engine that is a DCC + film compositor + DAW + game engine —
which codecs, in what order, what is legally buildable in 2026, and how does "convert anything into anything" fall out?

## TL;DR

- **The foundation is already strong:** PNG · baseline JPEG · TGA · BMP · Radiance HDR · PFM · OpenEXR · DEFLATE/zlib —
  all ours, all gated. The `LdrImage`/`HdrImage` types + `ldr_decode` dispatch are the intermediate representation the
  whole platform converges on.
- **The legal map decides the roster.** Fully royalty-free (build everything): AV1/AVIF · VP9 · VP8/WebP · Opus · Vorbis
  · FLAC · MP3 (free since 2017) · MPEG-2 (last patent expires 2026) · GIF (LZW expired 2004) · TIFF. Time-gated: H.264
  (last patent 2027-11; Baseline profile royalty-free by design → Baseline decode now, full profiles post-expiry).
  EXCLUDED BY NAME: H.265/HEVC + AAC (actively licensed pools — transcode routes around them via AV1/Opus).
- **The architecture is decode → OWNED INTERMEDIATE → encode.** Images: LdrImage/HdrImage (exists). Audio: f32
  interleaved PCM (`AudioPcm`). Video: LdrImage frame sequences + PCM (which is ALSO the film-pipeline native form —
  EXR/PNG sequences). "Convert one into another" = any decoder × any encoder through the intermediates + resamplers —
  ONE transcode engine, not N² paths.

## The honest size ladder (why the sequencing below)

| Tier | Codecs | Each costs |
|---|---|---|
| days | GIF · TIFF-baseline · PNG/JPEG/TGA/BMP ENCODERS · WAV/AIFF · MP4/MKV/AVI containers · MJPEG video (rides our JPEG) | the codecs we just shipped were this tier |
| ~1-2 weeks | JPEG-progressive · FLAC (decode+encode) · MP3 decode · Vorbis decode · VP8L (WebP-lossless) · MPEG-2 | real DSP/entropy work, bounded specs |
| ~2-4 weeks | VP8-intra (WebP-lossy) · Opus decode · MP3 encode (psychoacoustics) · H.264-Baseline decode | serious codecs, still tractable solo |
| months | VP9 decode · AV1-intra (AVIF) · AV1 video · Opus encode · VP9/AV1 ENCODE · H.264 full | the mountains — scheduled, not denied |

## Recommendation — the MED band (D-007), sequenced by value-per-effort

**Sub-band A — complete the image story (MED-1..4):**
- **MED-1** GIF (LZW + animation frames + disposal methods — the first ANIMATED resource, feeds GEO-9's timeline) +
  TIFF baseline (II/MM endianness, strips+tiles, uncompressed/PackBits/LZW/Deflate — our inflate; the CAD/scan world) +
  JPEG **progressive** (SOF2 — closing our named-Unsupported).
- **MED-2** image ENCODERS: PNG (our deflate; filters + heuristic) · JPEG baseline (our tables, quality knob) · TGA/BMP
  write · EXR write exists — plus the first `convert` verb: ANY LdrImage/HdrImage in → ANY encoder out (+resize/tone-map
  edges later). This is where "convert one into another" starts being real.
- **MED-3** WebP: VP8L lossless first (self-contained spec), then VP8-intra lossy (bool coder + intra prediction + the
  loop filter).
- **MED-4** AVIF = AV1-INTRA decode (the image half of the AV1 mountain: OBU/tile parsing, symbol coder, intra modes,
  transforms, loop restoration). Deliberately AFTER WebP — VP8 is the training ground for the AV1 family. LARGE.

**Sub-band B — the audio codec story (MED-5..7; the GEO-10 `crd-audio` module consumes these):**
- **MED-5** WAV/AIFF (containers) + **FLAC decode + encode** (rice + LPC — lossless round-trip bit-exact gates) → the
  `AudioPcm` intermediate (f32 interleaved + rate/channels) + resampler (hesap-dsp — already ours).
- **MED-6** lossy DECODE: **MP3** (patent-free; Huffman + IMDCT + synthesis filterbank) · **Vorbis** · **Opus** (SILK+
  CELT — the modern standard, Discord/WebRTC/Teams all ride it). AAC decode EXCLUDED BY NAME (active pool).
- **MED-7** lossy ENCODE: **Opus encode** (open, the one lossy encoder worth owning — LARGE) + MP3 encode (patent-free
  since 2017; psychoacoustic model). AAC encode EXCLUDED BY NAME.

**Sub-band C — the video story (MED-8..11):**
- **MED-8** containers: **MP4/ISO-BMFF** (box parse + mux) · **MKV/WebM** (EBML) · AVI (RIFF) — demux to codec packets,
  mux from them; the container layer is codec-independent and unblocks everything below.
- **MED-9** video decode I — the pragmatic wins: **MJPEG** (each frame IS our JPEG — near-free) · **image-SEQUENCE video**
  (EXR/PNG sequences as a first-class VideoResource — the FILM-pipeline native form GEO-9 already masters to) ·
  **MPEG-2** (patents done 2026; DVD/broadcast archives).
- **MED-10** video decode II: **VP9** → **AV1** (sharing MED-4's intra core + adding inter prediction/motion) ·
  **H.264-Baseline** decode now (royalty-free by design), full profiles at the 2027-11 expiry — the date is IN the slice.
  H.265 EXCLUDED BY NAME.
- **MED-11** video ENCODE: image-sequence→MJPEG + MPEG-2 encode first (real deliverable files from the GEO-9 timeline
  render), then VP9/AV1 encode (the largest single item in the band — rate control, RDO; scheduled last).

**Sub-band D — the platform (MED-12):**
- **MED-12** the TRANSCODE ENGINE + resource integration: decode→intermediate→encode over ALL of the above; streamed
  `VideoResource`/`AudioStreamResource` (the resource system's streaming loader exists); `ceridc convert` (any→any with
  explicit quality/rate knobs, JSON reports — the GEO-11 agent surface); GEO-9 timelines REFERENCE these resources,
  GEO-10's audio graph plays them. Conversion correctness gates: lossless round-trips BIT-EXACT (FLAC↔WAV, PNG↔TGA…),
  lossy paths PSNR-gated against the source.

## The intermediates (the whole platform converges here)

- **Images:** `LdrImage` (RGBA8) + `HdrImage` (f32) — exist. Add `ImageSeq` (frames + rational timestamps) for GIF/video.
- **Audio:** `AudioPcm` — f32 interleaved, sample rate + channel count + channel layout; hesap-dsp resamples/mixes.
- **Video:** demuxed packet streams (container layer) ↔ `ImageSeq` + `AudioPcm` (codec layer). Rational time everywhere
  (the GEO-9 rule — 1001/30000 exact, never float seconds).

## What we read

- [H.264 patent status](https://www.osnews.com/story/24954/us-patent-expiration-for-mp3-mpeg-2-h264/) + [Via-LA AVC pool](https://www.via-la.com/licensing-programs/avc-h-264/) — last AVC patent US 7826532 expires 2027-11-29; the pool still licenses new entrants in 2026; **Baseline profile was designed royalty-free**. → Baseline decode now, full at expiry, date recorded in the slice.
- [MP3 patents expired 2017](https://appleinsider.com/articles/17/05/15/patent-licensing-on-mp3-format-expires-apple-preferred-aac-now-a-de-facto-standard) + [Fraunhofer on mp3 licensing end](https://www.audioblog.iis.fraunhofer.com/mp3-software-patents-licenses) — MP3 decode AND encode are fully free.
- [AAC licensing](https://macprovideo.com/article/audio-software/mp3-is-dead-expired-patents-and-why-aac-is-a-better-compressed-audio-format) — per-unit pool, active → EXCLUDED BY NAME.
- [Opus](https://opus-codec.org/) + [OpusFAQ](https://wiki.xiph.org/OpusFAQ) — totally open/royalty-free; the modern lossy standard (WebRTC/Discord/Teams/Zoom). The lossy codec worth owning end-to-end.
- MPEG-2: last pool patent (7334248) expires 2026 — the archival/broadcast decode is clear.

## Alternatives considered

- **FFmpeg/libav as the media layer** — rejected: the zero-3rd-party doctrine is the whole point (we just retired cgltf
  and are retiring stb); FFmpeg would be the largest foreign surface in the engine. It DOES serve as the perfect
  transcode TEST ORACLE (compare our decodes against ffmpeg output in gates) — oracle-only, like mikktspace.c.
- **Licensing H.265/AAC** — rejected: royalty pools are per-unit poison for an engine platform; AV1+Opus beat them
  technically anyway. Routes around: transcode offers AV1/Opus/VP9/MP3 targets.
- **"Video = wrap the OS decoder (Media Foundation/VideoToolbox)"** — rejected for the product path (platform-dependent
  results break bit-honest transcode gates + portability); MAYBE later as an optional hardware-decode fast path clearly
  separated from the owned reference path.

## Pitfalls / gotchas

- **Codec correctness is conformance-vector work**: for VP8/VP9/AV1/H.264/Opus, hermetic hand-built fixtures are not
  enough — the slices must pull the OFFICIAL conformance vectors (ivf/obu test suites, Opus test vectors) into the gates.
- **The bool/arithmetic coders are the bug nests** (VP8 bool coder, AV1 MSAC, CABAC): implement + gate them STANDALONE
  first (bit-exact against spec pseudocode traces) before any pixel work.
- **Audio realtime vs decode**: decoders produce PCM offline; the GEO-10 realtime callback consumes preloaded/streamed
  buffers only — never decode on the audio thread.
- **Rational timestamps from day one** in containers (MP4 timescales, MKV timecode scale) — float seconds here poisons
  the GEO-9 timeline exactness.
- **Patent dates are jurisdiction-specific** — the H.264 note tracks US expiry; ship-region review belongs to release
  process, and the slice records the date so it is a checkable fact, not folklore.

## Next research

1. VP8/VP8L bitstream deep-dive (MED-3 entry) — the bool coder + prediction modes, with the RFC 6386 spec as the base.
2. AV1 decode architecture (MED-4/10) — OBU/tiles/symbol coder; what intra-only (AVIF) actually requires vs full video.
3. Opus internals (MED-6/7) — SILK/CELT split, the range coder, the MDCT path.
