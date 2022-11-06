#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <system_error>
#include <vector>
#include <algorithm>


using namespace std;

#pragma pack(1)

struct MFMIMG
{
	uint8_t headername[7];

	uint16_t number_of_track;
	uint8_t number_of_side;

	uint16_t floppyRPM;
	uint16_t floppyBitRate;
	uint8_t floppyiftype;

	uint32_t mfmtracklistoffset;
};

struct MFMTRACKIMG
{
	uint16_t track_number;
	uint8_t side_number;
	uint32_t mfmtracksize;
	uint32_t mfmtrackoffset;
};

#pragma pack()


// TODO: Genericise find_mark as find_mark(pattern, bits)

// an MFM track
class MFMTrack : public vector<bool> {
	public:
		off_t find_mark(const uint64_t mark, const size_t len, const size_t start=0);
		uint16_t track;
		uint8_t side;
};


off_t MFMTrack::find_mark(const uint64_t mark, const size_t len, const size_t start)
{
	vector<bool> patt;

	uint32_t x = mark;
	for (size_t i=0; i<len; i++) {
		patt.push_back(x & (1<<(len-1)));
		x <<= 1;
	}
#if 0
	for (auto v : patt) {
		printf("%d", v ? 1 : 0);
	}
	printf("\n");

	printf("len this %lu\n", this->size());
	printf("len patt %lu\n", patt.size());
#endif

	auto res = std::search(this->begin()+start, this->end(), patt.begin(), patt.end());
	if (res == this->end()) {
		return -1;
	} else {
		return std::distance(this->begin(), res);
	}
}



// an MFM file
class MFMFile {
	public:
		MFMFile(const char *filename);
		uint16_t rpm;
		uint16_t bitrate;
		vector<MFMTrack> tracks;
};

MFMFile::MFMFile(const char *filename)
{
	FILE *fp;

	// open input file
	fp = fopen(filename, "rb");
	if (fp == NULL) {
		throw system_error(-1, generic_category(), "bad file");
	}

	// read and check header
	MFMIMG hdr;
	printf("sizes - MFMIMG is %lu, MFMTRACKIMG is %lu\n", sizeof(MFMIMG), sizeof(MFMTRACKIMG));
	if (fread(&hdr, 1, sizeof(MFMIMG), fp) != sizeof(MFMIMG)) {
		throw system_error(-1, generic_category(), string("bad header"));
	}
	
	const char *HDRSIG = "HXCMFM";
	if (memcmp(hdr.headername, HDRSIG, strlen(HDRSIG)+1) != 0) {
		throw system_error(-1, generic_category(), "bad magic");
	}

	// load header fields
	this->rpm = hdr.floppyRPM;
	this->bitrate = hdr.floppyBitRate;

	// load the track list
	vector<MFMTRACKIMG> tracklist;
	tracklist.resize(hdr.number_of_side * hdr.number_of_track);
	
	fseek(fp, hdr.mfmtracklistoffset, SEEK_SET);
	for (size_t n = 0; n < hdr.number_of_side * hdr.number_of_track; n++) {
		if (fread(&tracklist[n], 1, sizeof(MFMTRACKIMG), fp) != sizeof(MFMTRACKIMG)) {
			throw system_error(-1, generic_category(), "error reading tracklist");
		}
	}

	// load the tracks
	this->tracks.resize(tracklist.size());

	size_t trkn=0;
	vector<uint8_t> trackbuf;
	for (auto &tlent : tracklist) {
		// read track data (bytes)
		trackbuf.resize(tlent.mfmtracksize);
		fseek(fp, tlent.mfmtrackoffset, SEEK_SET);
		if (fread(&trackbuf[0], sizeof(MFMTrack::value_type), tlent.mfmtracksize, fp) != tlent.mfmtracksize) {
			throw system_error(-1, generic_category(), "error reading trackdata");
		}

		// convert to MFMTrack (bit vector)
		// MFM file contains bits msb-first
		auto &trkbits = this->tracks[trkn];
		trkn++;
		for (auto bval : trackbuf) {
			for (size_t i=0; i<8; i++) {
				if (bval & 0x80) {
					trkbits.push_back(1);
				} else {
					trkbits.push_back(0);
				}

				bval <<= 1;
			}
		}
		//printf("cyl %u hd %u len %lu\n", tlent.track_number, tlent.side_number, trkbits.size());
		trkbits.track = tlent.track_number;
		trkbits.side = tlent.side_number;
	}

	fclose(fp);
}



//const uint16_t PREGAP_MARK = 0xAAAA;
//const uint16_t SYNC_MARK   = 0x9125;

const uint64_t PREGAP_THEN_SYNCMARK = 0xAAAA9125;
const size_t   PATTLEN = 2*16;
const size_t   BLANKING = 500;	// don't search for sync before this bit

int main(int argc, char **argv)
{
	// check params
	if (argc < 2) {
		fprintf(stderr, "syntax: %s mfmfile\n", argv[0]);
		return -1;
	}

	MFMFile mfm(argv[1]);

	FILE *fo = fopen("out.bin", "wb");

	uint8_t buf[9984];
	for (auto track : mfm.tracks) {
		// Quantel floppy "tracks" are 19200 bytes long.
		// This is 9984 (0x2700) bytes on head 0 and 9216 (0x2400) on head 1
		//
		// On a hard drive we'd have 19200 bytes per cylinder, the same on every head
		//
		// To the Paintbox, a floppy drive is a removable hard drive with 74 tracks
		// and 1 head.
		size_t tracklen = track.side == 0 ? 0x2700 : 0x2400;

		// Fill the track with nulls to start (in case we need to write a blank track)
		memset(buf, '\0', tracklen);

		// Find the sync mark
		auto syncpos = track.find_mark(PREGAP_THEN_SYNCMARK, PATTLEN, BLANKING);
		//printf("find Pregap+Sync %ld = %ld bytes\n", syncpos, (track.size() - syncpos)/16);

		// Sanity check the sync mark
		if (syncpos == -1) {
			printf("%02d/%d - Blank track\n", track.track, track.side);
		} 
		else if ((track.size() - syncpos) < (9400 * 16)) {
			printf("%02d/%d - Sync too late\n", track.track, track.side);
		}
		else {
			printf("%02d/%d - Track has %lu mfmbits with pg+sync at %lu -- (about %lu bytes total, or %lu after pregap+sync), tracklen is 0x%lX -- ",
					track.track, track.side,
					track.size(),
					syncpos,
					track.size()/16,
					(track.size() - syncpos)/16,
					tracklen);

			// decode MFM bits back into binary by dropping the clock bits
			uint8_t byte;
			for (size_t i=0; i<tracklen+4; i++) {
				byte = 0;
				for (size_t nbit=0; nbit<8; nbit++) {
					byte <<= 1;
					//  syncpos: where we found the sync burst (pregap then syncbyte)
					//  32:   length of the sync burst
					//  i*16: the byte offset
					//  nbit*2: the data bit we're currently looking at (*2 so we skip clock bits)
					//  +1:   the first mfmbit is a clock bit, skip to the first data bit
					if (track.at(syncpos+32+(i*16)+(nbit*2)+1)) {
						byte |= 1;
					}
				}
				buf[i] = byte;
			}

			// Check the sentinel pattern
			printf("EOT is %02X:%02X:%02X:%02X\n",
					buf[tracklen],
					buf[tracklen+1],
					buf[tracklen+2],
					buf[tracklen+3]);
		}

		// Save the decoded track
		fwrite(buf, 1, tracklen, fo);
	}

	fclose(fo);


	return 0;
}
