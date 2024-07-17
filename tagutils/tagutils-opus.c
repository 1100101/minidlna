//=========================================================================
// FILENAME	: tagutils-opus.c
// DESCRIPTION	: Opus metadata reader
//=========================================================================

static int
_get_opusfileinfo(char *filename, struct song_metadata *psong)
{
	OggOpusFile *opusfile;
	const OpusTags *tags;
	char **comment;
	int *commentlen;
	int j, e;


	opusfile = op_open_file (filename, &e);
	if(!opusfile)
	{
		DPRINTF(E_WARN, L_SCANNER,
			"Error opening input file \"%s\": %s\n", filename, opus_strerror(e));
		return -1;
	}

	DPRINTF(E_MAXDEBUG, L_SCANNER, "Processing file \"%s\"...\n", filename);

	psong->song_length = op_pcm_total (opusfile, -1);
	if (psong->song_length < 0)
	{
		DPRINTF(E_WARN, L_SCANNER,
				"Unable to obtain length of %s\n", filename);
		psong->song_length = 0;
	} else
		/* Sample rate is always 48k, so length in ms is just samples/48 */
		psong->song_length /= 48;

	/* Note that this gives only the first link's channel count. */
	psong->channels = op_channel_count (opusfile, -1);

	psong->samplerate = 48000;
	psong->bitrate = op_bitrate (opusfile, -1);

	tags = op_tags (opusfile, -1);

	if (!tags)
	{
		DPRINTF(E_WARN, L_SCANNER, "Unable to obtain tags from %s\n",
				filename);
		return -1;
	}

	comment = tags->user_comments;
	commentlen = tags->comment_lengths;

	for (j = 0; j < tags->comments; j++)
		vc_scan (psong, *(comment++), *(commentlen++));

	op_free (opusfile);
	return 0;
}
