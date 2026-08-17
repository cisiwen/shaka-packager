HLS options
^^^^^^^^^^^

--hls_master_playlist_output <file_path>

    Output path for the master playlist for HLS. This flag must be used to
    output HLS.

--hls_base_url <url>

    The base URL for the Media Playlists and media files listed in the
    playlists. This is the prefix for the files.

--hls_key_uri <uri>

    The key uri for 'identity' and 'com.apple.streamingkeydelivery' (FairPlay)
    key formats. Ignored if the playlist is not encrypted or not using the above
    key formats.

--hls_playlist_type <type>

    VOD, EVENT, or LIVE. This defines the EXT-X-PLAYLIST-TYPE in the HLS
    specification. For hls_playlist_type of LIVE, EXT-X-PLAYLIST-TYPE tag is
    omitted.

--time_shift_buffer_depth <seconds>

    Guaranteed duration of the time shifting buffer for LIVE playlists, in
    seconds.

--preserved_segments_outside_live_window <num_segments>

    Segments outside the live window (defined by `time_shift_buffer_depth`
    above) are automatically removed except for the most recent X segments
    defined by this parameter. This is needed to accommodate latencies in
    various stages of content serving pipeline, so that the segments stay
    accessible as they may still be accessed by the player.

    The segments are not removed if the value is zero.

--default_language <language>

    The first audio/text rendition in a group tagged with this language will
    have 'DEFAULT' attribute set to 'YES'. This allows the player to choose the
    correct default language for the content.

    This applies to both audio and text tracks. The default language for text
    tracks can be overriden by  'default_text_language'.

--default_text_language <text_language>

    Same as above, but this applies to text tracks only, and overrides the
    default language for text tracks.

--hls_media_sequence_number <unsigned_number>

    HLS uses the EXT-X-MEDIA-SEQUENCE tag at the start of a live playlist in
    order to specify the first segment sequence number. This is because any
    live playlist have a limited number of segments, and they also keep
    updating with new segments while removing old ones. When a player refreshes
    the playlist, this information is important for keeping track of segments
    positions.

    When the packager starts, it naturally starts this count from zero. However,
    there are many situations where the packager may be restarted, without this
    meaning starting this value from zero (but continuing a previous sequence).
    The most common situations are problems in the encoder feeding the packager.

    With those cases in mind, this parameter allows to set the initial
    EXT-X-MEDIA-SEQUENCE value. This way, it's possible to continue the sequence
    number from previous packager run.

    For more information about the reasoning of this, please see issue
    `#691 <https://github.com/shaka-project/shaka-packager/issues/691>`_.

    The EXT-X-MEDIA-SEQUENCE documentation can be read here:
    https://tools.ietf.org/html/rfc8216#section-4.3.3.2.

--hls_start_time_offset <seconds>

    Sets EXT-X-START on the media playlists to specify the preferred point
    at wich the player should start playing.
    A positive number indicates a time offset from the beginning of the playlist.
    A negative number indicates a negative time offset from the end of the
    last media segment in the playlist.

--hls_only=0|1

    Optional. Defaults to 0 if not specified. If it is set to 1, indicates the
    stream is HLS only.

--force_cl_index

    True forces the muxer to order streams in the order given 
    on the command-line. False uses the previous unordered behavior.

--create_session_keys

    Playback of Offline HLS assets shall use EXT-X-SESSION-KEY to declare all
    eligible content keys in the master playlist.

--hls_rotate_manifest_hourly

    For live HLS output, additionally rotate to a brand new, independent,
    self-contained HLS media/master playlist file set at every UTC
    wall-clock hour boundary (00:00, 01:00, ... UTC), alongside (not
    instead of) the normal, always-current, sliding-window live manifest
    (``--hls_master_playlist_output`` and its per-stream playlists). This
    is meant for building a DVR-style session recording: while the live
    manifest keeps behaving exactly as it always has, an additional
    "archive" copy of the manifest and segments is rotated hourly, with
    each hourly file getting its own ``#EXT-X-ENDLIST`` and its own
    ``EXT-X-MEDIA-SEQUENCE``/``EXT-X-DISCONTINUITY-SEQUENCE`` starting at
    0 when closed. Segment (``.m4s``) production itself is completely
    unaffected -- only ``.m3u8`` playlist writing rotates. Requires
    ``--hls_session_index_output`` to also be set. DASH/MPD output
    (``--mpd_output``) is unaffected by this flag; it has no archive
    concept.

--hls_session_index_output <file_path>

    Output path for a JSON "session index" file listing every hourly
    master playlist produced by ``--hls_rotate_manifest_hourly``, e.g.::

        {"segments": [
          {"start": "2026-08-14T15:00:00Z", "master_playlist": "master_2026-08-14T15.m3u8"},
          {"start": "2026-08-14T16:00:00Z", "master_playlist": "master_2026-08-14T16.m3u8"}
        ]}

    This lets a DVR-style tool browse a whole live session as a sequence
    of hour-long recordings. Required (and only used) when
    ``--hls_rotate_manifest_hourly`` is set.

--hls_manifest_rotation_test_interval_seconds <seconds>

    **Test-only.** If nonzero, dilates wall-clock time so that manifest
    rotation happens roughly every N real seconds instead of on genuine
    UTC hour boundaries, letting ``--hls_rotate_manifest_hourly`` be
    exercised in short integration tests without waiting for a real hour
    to elapse. Must be 0 (the default) in production.