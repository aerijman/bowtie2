/*
 * Copyright 2011, Ben Langmead <langmea@cs.jhu.edu>
 *
 * This file is part of Bowtie 2.
 *
 * Bowtie 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bowtie 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bowtie 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PAT_H_
#define PAT_H_

#include <stdio.h>
#include <sys/stat.h>
#include <zlib.h>
#include <cassert>
#include <string>
#include <ctype.h>
#include "alphabet.h"
#include "assert_helpers.h"
#include "random_source.h"
#include "threading.h"
#include "qual.h"
#include "search_globals.h"
#include "sstring.h"
#include "ds.h"
#include "read.h"
#include "util.h"

#ifdef _WIN32
#define getc_unlocked _fgetc_nolock
#endif

/**
 * Classes and routines for reading reads from various input sources.
 */

/**
 * Parameters affecting how reads and read in.
 */
struct PatternParams {
	
	PatternParams() { }

	PatternParams(
		int format_,
		bool fileParallel_,
		uint32_t seed_,
		size_t max_buf_,
		bool solexa64_,
		bool phred64_,
		bool intQuals_,
		int trim5_,
		int trim3_,
		int sampleLen_,
		int sampleFreq_,
		size_t skip_,
		int nthreads_,
		bool fixName_) :
		format(format_),
		fileParallel(fileParallel_),
		seed(seed_),
		max_buf(max_buf_),
		solexa64(solexa64_),
		phred64(phred64_),
		intQuals(intQuals_),
		trim5(trim5_),
		trim3(trim3_),
		sampleLen(sampleLen_),
		sampleFreq(sampleFreq_),
		skip(skip_),
		nthreads(nthreads_),
		fixName(fixName_) { }

	int format;			  // file format
	bool fileParallel;	  // true -> wrap files with separate PatternComposers
	uint32_t seed;		  // pseudo-random seed
	size_t max_buf;		  // number of reads to buffer in one read
	bool solexa64;		  // true -> qualities are on solexa64 scale
	bool phred64;		  // true -> qualities are on phred64 scale
	bool intQuals;		  // true -> qualities are space-separated numbers
	int trim5;            // amt to hard clip from 5' end
	int trim3;            // amt to hard clip from 3' end
	int sampleLen;		  // length of sampled reads for FastaContinuous...
	int sampleFreq;		  // frequency of sampled reads for FastaContinuous...
	size_t skip;		  // skip the first 'skip' patterns
	int nthreads;		  // number of threads for locking
	bool fixName;		  //
};

/**
 * All per-thread storage for input read data.
 */
struct PerThreadReadBuf {
	
	PerThreadReadBuf(size_t max_buf) :
		max_buf_(max_buf),
		bufa_(max_buf),
		bufb_(max_buf),
		rdid_()
	{
		bufa_.resize(max_buf);
		bufb_.resize(max_buf);
		reset();
	}
	
	Read& read_a() { return bufa_[cur_buf_]; }
	Read& read_b() { return bufb_[cur_buf_]; }
	
	const Read& read_a() const { return bufa_[cur_buf_]; }
	const Read& read_b() const { return bufb_[cur_buf_]; }
	
	/**
	 * Return read id for read/pair currently in the buffer.
	 */
	TReadId rdid() const {
		assert_neq(rdid_, std::numeric_limits<TReadId>::max());
		return rdid_ + cur_buf_;
	}
	
	/**
	 * Reset state as though no reads have been read.
	 */
	void reset() {
		cur_buf_ = bufa_.size();
		for(size_t i = 0; i < max_buf_; i++) {
			bufa_[i].reset();
			bufb_[i].reset();
		}
		rdid_ = std::numeric_limits<TReadId>::max();
	}
	
	/**
	 * Advance cursor to next element
	 */
	void next() {
		assert_lt(cur_buf_, bufa_.size());
		cur_buf_++;
	}
	
	/**
	 * Return true when there's nothing left for next().
	 */
	bool exhausted() {
		assert_leq(cur_buf_, bufa_.size());
		return cur_buf_ >= bufa_.size()-1 || bufa_[cur_buf_+1].readOrigBuf.empty();
	}
	
	/**
	 * Just after a new batch has been loaded, use init to
	 * set cur_buf_ appropriately.
	 */
	void init() {
		cur_buf_ = 0;
	}
	
	/**
	 * Set read id of first read in buffer.
	 */
	void setReadId(TReadId rdid) {
		rdid_ = rdid;
	}
	
	const size_t max_buf_; // max # reads to read into buffer at once
	EList<Read> bufa_;	   // Read buffer for mate as
	EList<Read> bufb_;	   // Read buffer for mate bs
	size_t cur_buf_;	   // Read buffer currently active
	TReadId rdid_;		   // index of read at offset 0 of bufa_/bufb_
};

extern void wrongQualityFormat(const BTString& read_name);
extern void tooFewQualities(const BTString& read_name);
extern void tooManyQualities(const BTString& read_name);

/**
 * Encapsulates a synchronized source of patterns; usually a file.
 * Optionally reverses reads and quality strings before returning them,
 * though that is usually more efficiently done by the concrete
 * subclass.  Concrete subclasses should delimit critical sections with
 * calls to lock() and unlock().
 */
class PatternSource {
	
public:
	
	PatternSource(const PatternParams& p) :
		pp_(p),
		readCnt_(0),
		mutex() { }
	
	virtual ~PatternSource() { }
	
	/**
	 * Implementation to be provided by concrete subclasses.  An
	 * implementation for this member is only relevant for formats
	 * where individual input sources look like single-end-read
	 * sources, e.g., formats where paired-end reads are specified in
	 * parallel read files.
	 */
	virtual std::pair<bool, int> nextBatch(
		PerThreadReadBuf& pt,
		bool batch_a,
		bool lock = true) = 0;
	
	/**
	 * Finishes parsing a given read.  Happens outside the critical section.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) const = 0;
	
	/**
	 * Reset so that next call to nextBatch* gets the first batch.
	 */
	virtual void reset() { readCnt_ = 0; }
	
	/**
	 * Return a new dynamically allocated PatternSource for the given
	 * format, using the given list of strings as the filenames to read
	 * from or as the sequences themselves (i.e. if -c was used).
	 */
	static PatternSource* patsrcFromStrings(
		const PatternParams& p,
		const EList<std::string>& qs);
	
	/**
	 * Return number of reads light-parsed by this stream so far.
	 */
	TReadId readCount() const { return readCnt_; }
	
protected:
	
	
	// Reference to global input-parsing parameters
	const PatternParams& pp_;
	
	// The number of reads read by this PatternSource
	volatile TReadId readCnt_;
	
	// Lock enforcing mutual exclusion for (a) file I/O, (b) writing fields
	// of this or another other shared object.
	MUTEX_T mutex;
};

/**
 * Encapsulates a source of patterns which is an in-memory vector.
 */
class VectorPatternSource : public PatternSource {

public:

	/**
	 * Populate member lists, v_, quals_, names_, etc, with information parsed
	 * from the given list of strings.
	 */
	VectorPatternSource(
		const EList<std::string>& v,
		const PatternParams& p);
	
	virtual ~VectorPatternSource() { }
	
	/**
	 * Read next batch.  However, batch concept is not very applicable for this
	 * PatternSource where all the info has already been parsed into the fields
	 * in the contsructor.	This essentially modifies the pt as though we read
	 * in some number of patterns.
	 */
	virtual std::pair<bool, int> nextBatch(
		PerThreadReadBuf& pt,
		bool batch_a,
		bool lock = true);
	
	/**
	 * Reset so that next call to nextBatch* gets the first batch.
	 */
	virtual void reset() {
		PatternSource::reset();
		cur_ = skip_;
		paired_ = false;
	}

	/**
	 * Finishes parsing outside the critical section
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) const;
	
private:

	pair<bool, int> nextBatchImpl(
		PerThreadReadBuf& pt,
		bool batch_a);

	size_t cur_;			   // index for first read of next batch
	size_t skip_;			   // # reads to skip
	bool paired_;			   // whether reads are paired
	EList<string> tokbuf_;	   // buffer for storing parsed tokens
	EList<Read::TBuf> bufs_;   // per-read buffers
	char nametmp_[20];		   // temp buffer for constructing name
};

/**
 * Parent class for PatternSources that read from a file.
 * Uses unlocked C I/O, on the assumption that all reading
 * from the file will take place in an otherwise-protected
 * critical section.
 */
class CFilePatternSource : public PatternSource {
public:
	CFilePatternSource(
		const EList<std::string>& infiles,
		const PatternParams& p) :
		PatternSource(p),
		infiles_(infiles),
		filecur_(0),
		fp_(NULL),
		zfp_(NULL),
		is_open_(false),
		skip_(p.skip),
		first_(true),
		compressed_(false)
	{
		assert_gt(infiles.size(), 0);
		errs_.resize(infiles_.size());
		errs_.fill(0, infiles_.size(), false);
		open(); // open first file in the list
		filecur_++;
	}

	/**
	 * Close open file.
	 */
	virtual ~CFilePatternSource() {
		if(is_open_) {
			if (compressed_) {
				assert(zfp_ != NULL);
				gzclose(zfp_);
			}
			else {
				assert(fp_ != NULL);
				fclose(fp_);
			}
		}
	}

	/**
	 * Fill Read with the sequence, quality and name for the next
	 * read in the list of read files.	This function gets called by
	 * all the search threads, so we must handle synchronization.
	 *
	 * Returns pair<bool, int> where bool indicates whether we're
	 * completely done, and int indicates how many reads were read.
	 */
	virtual std::pair<bool, int> nextBatch(
		PerThreadReadBuf& pt,
		bool batch_a,
		bool lock = true);
	
	/**
	 * Reset so that next call to nextBatch* gets the first batch.
	 * Should only be called by the master thread.
	 */
	virtual void reset() {
		PatternSource::reset();
		filecur_ = 0,
		open();
		filecur_++;
	}

protected:

	/**
	 * Light-parse a batch of unpaired reads from current file into the given
	 * buffer.	Called from CFilePatternSource.nextBatch().
	 */
	virtual std::pair<bool, int> nextBatchFromFile(
		PerThreadReadBuf& pt,
		bool batch_a,
		unsigned read_idx) = 0;

	/**
	 * Reset state to handle a fresh file
	 */
	virtual void resetForNextFile() { }
	
	/**
	 * Open the next file in the list of input files.
	 */
	void open();

	int getc_wrapper() {
		return compressed_ ? gzgetc(zfp_) : getc_unlocked(fp_);
	}

	int ungetc_wrapper(int c) {
		return compressed_ ? gzungetc(c, zfp_) : ungetc(c, fp_);
	}

	bool is_gzipped_file(const std::string& filename) {
		struct stat s;
		if (stat(filename.c_str(), &s) != 0) {
			perror("stat");
		}
		else {
			if (S_ISFIFO(s.st_mode))
				return true;
		}
		size_t pos = filename.find_last_of(".");
		std::string ext = (pos == std::string::npos) ? "" : filename.substr(pos + 1);
		if (ext == "" || ext == "gz" || ext == "Z") {
			return true;
		}
		return false;
	}
	
	EList<std::string> infiles_;	 // filenames for read files
	EList<bool> errs_;		 // whether we've already printed an error for each file
	size_t filecur_;		 // index into infiles_ of next file to read
	FILE *fp_;			 // read file currently being read from
	gzFile zfp_;			 // compressed version of fp_
	bool is_open_;			 // whether fp_ is currently open
	TReadId skip_;			 // number of reads to skip
	bool first_;			 // parsing first record in first file?
	char buf_[64*1024];		 // file buffer
	bool compressed_;

private:

	pair<bool, int> nextBatchImpl(
		PerThreadReadBuf& pt,
		bool batch_a);
};

/**
 * Synchronized concrete pattern source for a list of FASTA files.
 */
class FastaPatternSource : public CFilePatternSource {

public:
	
	FastaPatternSource(
		const EList<std::string>& infiles,
		const PatternParams& p) :
		CFilePatternSource(infiles, p),
		first_(true) { }
	
	/**
	 * Reset so that next call to nextBatch* gets the first batch.
	 * Should only be called by the master thread.
	 */
	virtual void reset() {
		first_ = true;
		CFilePatternSource::reset();
	}
	
	/**
	 * Finalize FASTA parsing outside critical section.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) const;

protected:

	/**
	 * Light-parse a FASTA batch into the given buffer.
	 */
	virtual std::pair<bool, int> nextBatchFromFile(
		PerThreadReadBuf& pt,
		bool batch_a,
		unsigned read_idx);

	/**
	 * Scan to the next FASTA record (starting with >) and return the first
	 * character of the record (which will always be >).
	 */
	static int skipToNextFastaRecord(FileBuf& in) {
		int c;
		while((c = in.get()) != '>') {
			if(in.eof()) return -1;
		}
		return c;
	}
	
	/**
	 * Reset state to handle a fresh file
	 */
	virtual void resetForNextFile() {
		first_ = true;
	}
	
	bool first_;
};

/**
 * Synchronized concrete pattern source for a list of files with tab-
 * delimited name, seq, qual fields (or, for paired-end reads,
 * basename, seq1, qual1, seq2, qual2).
 */
class TabbedPatternSource : public CFilePatternSource {

public:

	TabbedPatternSource(
		const EList<std::string>& infiles,
		const PatternParams& p,
		bool  secondName) :  // whether it's --12/--tab5 or --tab6
		CFilePatternSource(infiles, p),
		secondName_(secondName) { }

	/**
	 * Finalize tabbed parsing outside critical section.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) const;

protected:

	/**
	 * Light-parse a batch of tabbed-format reads into given buffer.
	 */
	virtual std::pair<bool, int> nextBatchFromFile(
		PerThreadReadBuf& pt,
		bool batch_a,
		unsigned read_idx);

	bool secondName_;	// true if --tab6, false if --tab5
};

/**
 * Synchronized concrete pattern source for Illumina Qseq files.  In
 * Qseq files, each read appears on a separate line and the tab-
 * delimited fields are:
 *
 * 1. Machine name
 * 2. Run number
 * 3. Lane number
 * 4. Tile number
 * 5. X coordinate of spot
 * 6. Y coordinate of spot
 * 7. Index: "Index sequence or 0. For no indexing, or for a file that
 *	  has not been demultiplexed yet, this field should have a value of
 *	  0."
 * 8. Read number: 1 for unpaired, 1 or 2 for paired
 * 9. Sequence
 * 10. Quality
 * 11. Filter: 1 = passed, 0 = didn't
 */
class QseqPatternSource : public CFilePatternSource {

public:

	QseqPatternSource(
		const EList<std::string>& infiles,
		const PatternParams& p) :
		CFilePatternSource(infiles, p) { }

	/**
	 * Finalize qseq parsing outside critical section.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) const;

protected:
	
	/**
	 * Light-parse a batch into the given buffer.
	 */
	virtual std::pair<bool, int> nextBatchFromFile(
		PerThreadReadBuf& pt,
		bool batch_a,
		unsigned read_idx);

	EList<std::string> qualToks_;
};

/**
 * Synchronized concrete pattern source for a list of FASTA files where
 * reads need to be extracted from long continuous sequences.
 */
class FastaContinuousPatternSource : public CFilePatternSource {
public:
	FastaContinuousPatternSource(
		const EList<std::string>& infiles,
		const PatternParams& p) :
		CFilePatternSource(infiles, p),
		length_(p.sampleLen),
		freq_(p.sampleFreq),
		eat_(length_-1),
		beginning_(true),
		bufCur_(0),
		cur_(0llu),
		last_(0llu)
	{
		assert_gt(freq_, 0);
		resetForNextFile();
		assert_leq(length_, 1024);
	}

	virtual void reset() {
		CFilePatternSource::reset();
		resetForNextFile();
	}

	/**
	 * Finalize FASTA parsing outside critical section.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) const;

protected:

	/**
	 * Light-parse a batch into the given buffer.
	 */
	virtual std::pair<bool, int> nextBatchFromFile(
		PerThreadReadBuf& pt,
		bool batch_a,
		unsigned read_idx);
	
	/**
	 * Reset state to be read for the next file.
	 */
	virtual void resetForNextFile() {
		eat_ = length_-1;
		name_prefix_buf_.clear();
		beginning_ = true;
		bufCur_ = 0;
		last_ = cur_;
	}

private:
	const size_t length_; /// length of reads to generate
	const size_t freq_;   /// frequency to sample reads
	size_t eat_;		/// number of characters we need to skip before
						/// we have flushed all of the ambiguous or
						/// non-existent characters out of our read
						/// window
	bool beginning_;	/// skipping over the first read length?
	char buf_[1024];	/// FASTA sequence buffer
	Read::TBuf name_prefix_buf_; /// FASTA sequence name buffer
	char name_int_buf_[20]; /// for composing offsets for names
	size_t bufCur_;		/// buffer cursor; points to where we should
						/// insert the next character
	uint64_t cur_;
	uint64_t last_;     /// number to subtract from readCnt_ to get
						/// the pat id to output (so it resets to 0 for
						/// each new sequence)
};

/**
 * Read a FASTQ-format file.
 * See: http://maq.sourceforge.net/fastq.shtml
 */
class FastqPatternSource : public CFilePatternSource {

public:

	FastqPatternSource(
		const EList<std::string>& infiles,
		const PatternParams& p, bool interleaved = false) :
		CFilePatternSource(infiles, p),
		first_(true),
		interleaved_(interleaved) { }
	
	virtual void reset() {
		first_ = true;
		CFilePatternSource::reset();
	}

	/**
	 * Finalize FASTQ parsing outside critical section.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) const;

protected:

	/**
	 * Light-parse a batch into the given buffer.
	 */
	virtual std::pair<bool, int> nextBatchFromFile(
		PerThreadReadBuf& pt,
		bool batch_a,
		unsigned read_idx);
	
	/**
	 * Reset state to be ready for the next file.
	 */
	virtual void resetForNextFile() {
		first_ = true;
	}

	bool first_;		// parsing first read in file
	bool interleaved_;	// fastq reads are interleaved
};

/**
 * Read a Raw-format file (one sequence per line).	No quality strings
 * allowed.  All qualities are assumed to be 'I' (40 on the Phred-33
 * scale).
 */
class RawPatternSource : public CFilePatternSource {

public:

	RawPatternSource(
		const EList<std::string>& infiles,
		const PatternParams& p) :
		CFilePatternSource(infiles, p), first_(true) { }

	virtual void reset() {
		first_ = true;
		CFilePatternSource::reset();
	}

	/**
	 * Finalize raw parsing outside critical section.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) const;

protected:

	/**
	 * Light-parse a batch into the given buffer.
	 */
	virtual std::pair<bool, int> nextBatchFromFile(
		PerThreadReadBuf& pt,
		bool batch_a,
		unsigned read_idx);
	
	/**
	 * Reset state to be ready for the next file.
	 */
	virtual void resetForNextFile() {
		first_ = true;
	}
	
private:
	
	bool first_;
};

/**
 * Abstract parent class for synhconized sources of paired-end reads
 * (and possibly also single-end reads).
 */
class PatternComposer {
public:
	PatternComposer(const PatternParams& p) : mutex_m() { }
	
	virtual ~PatternComposer() { }
	
	virtual void reset() = 0;
	
	/**
	 * Member function override by concrete, format-specific classes.
	 */
	virtual std::pair<bool, int> nextBatch(PerThreadReadBuf& pt) = 0;
	
	/**
	 * Make appropriate call into the format layer to parse individual read.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) = 0;
	
	/**
	 * Given the values for all of the various arguments used to specify
	 * the read and quality input, create a list of pattern sources to
	 * dispense them.
	 */
	static PatternComposer* setupPatternComposer(
		const EList<std::string>& si,	 // singles, from argv
		const EList<std::string>& m1,	 // mate1's, from -1 arg
		const EList<std::string>& m2,	 // mate2's, from -2 arg
		const EList<std::string>& m12,	 // both mates on each line, from --12
		const EList<std::string>& q,	 // qualities associated with singles
		const EList<std::string>& q1,	 // qualities associated with m1
		const EList<std::string>& q2,	 // qualities associated with m2
		const PatternParams& p,		// read-in params
		bool verbose);				// be talkative?
	
protected:
	
	static void free_EList_pmembers(const EList<PatternSource*>&);
	
	/// Lock enforcing mutual exclusion for (a) file I/O, (b) writing fields
	/// of this or another other shared object.
	MUTEX_T mutex_m;
};

/**
 * Encapsulates a synchronized source of both paired-end reads and
 * unpaired reads, where the paired-end must come from parallel files.
 */
class SoloPatternComposer : public PatternComposer {
	
public:
	
	SoloPatternComposer(
		const EList<PatternSource*>* src,
		const PatternParams& p) :
		PatternComposer(p),
		cur_(0),
		src_(src)
	{
		assert(src_ != NULL);
		for(size_t i = 0; i < src_->size(); i++) {
			assert((*src_)[i] != NULL);
		}
	}
	
	virtual ~SoloPatternComposer() {
		free_EList_pmembers(*src_);
		delete src_;
	}
	
	/**
	 * Reset this object and all the PatternSources under it so that
	 * the next call to nextBatch gets the very first read.
	 */
	virtual void reset() {
		for(size_t i = 0; i < src_->size(); i++) {
			(*src_)[i]->reset();
		}
		cur_ = 0;
	}
	
	/**
	 * Calls member functions of the individual PatternSource objects
	 * to get more reads.  Since there's no need to keep two separate
	 * files in sync (as there is for DualPatternComposer),
	 * synchronization can be handed by the PatternSource contained
	 * in the src_ field.
	 */
	virtual std::pair<bool, int> nextBatch(PerThreadReadBuf& pt);

	/**
	 * Make appropriate call into the format layer to parse individual read.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) {
		return (*src_)[0]->parse(ra, rb, rdid);
	}

protected:
	volatile size_t cur_; // current element in parallel srca_, srcb_ vectors
	const EList<PatternSource*>* src_; /// PatternSources for paired-end reads
};

/**
 * Encapsulates a synchronized source of both paired-end reads and
 * unpaired reads, where the paired-end must come from parallel files.
 */
class DualPatternComposer : public PatternComposer {
	
public:
	
	DualPatternComposer(
		const EList<PatternSource*>* srca,
		const EList<PatternSource*>* srcb,
		const PatternParams& p) :
		PatternComposer(p), cur_(0), srca_(srca), srcb_(srcb)
	{
		assert(srca_ != NULL);
		assert(srcb_ != NULL);
		// srca_ and srcb_ must be parallel
		assert_eq(srca_->size(), srcb_->size());
		for(size_t i = 0; i < srca_->size(); i++) {
			// Can't have NULL first-mate sources.	Second-mate sources
			// can be NULL, in the case when the corresponding first-
			// mate source is unpaired.
			assert((*srca_)[i] != NULL);
			for(size_t j = 0; j < srcb_->size(); j++) {
				assert_neq((*srca_)[i], (*srcb_)[j]);
			}
		}
	}
	
	virtual ~DualPatternComposer() {
		free_EList_pmembers(*srca_);
		delete srca_;
		free_EList_pmembers(*srcb_);
		delete srcb_;
	}
	
	/**
	 * Reset this object and all the PatternSources under it so that
	 * the next call to nextBatch gets the very first read.
	 */
	virtual void reset() {
		for(size_t i = 0; i < srca_->size(); i++) {
			(*srca_)[i]->reset();
			if((*srcb_)[i] != NULL) {
				(*srcb_)[i]->reset();
			}
		}
		cur_ = 0;
	}
	
	/**
	 * Calls member functions of the individual PatternSource objects
	 * to get more reads.  Since we need to keep the two separate
	 * files in sync, synchronization can be handed at this level, with
	 * one critical section surrounding both calls into the srca_ and
	 * srcb_ member functions.
	 */
	virtual std::pair<bool, int> nextBatch(PerThreadReadBuf& pt);

	/**
	 * Make appropriate call into the format layer to parse individual read.
	 */
	virtual bool parse(Read& ra, Read& rb, TReadId rdid) {
		return (*srca_)[0]->parse(ra, rb, rdid);
	}

protected:
	
	volatile size_t cur_; // current element in parallel srca_, srcb_ vectors
	const EList<PatternSource*>* srca_; // for 1st matesunpaired
	const EList<PatternSource*>* srcb_; // for 2nd mates
};

/**
 * Encapsulates a single thread's interaction with the PatternSource.
 * Most notably, this class holds the buffers into which the
 * PatterSource will write sequences.  This class is *not* threadsafe
 * - it doesn't need to be since there's one per thread.  PatternSource
 * is thread-safe.
 */
class PatternSourcePerThread {
	
public:
	
	PatternSourcePerThread(
		PatternComposer& composer,
		const PatternParams& pp) :
		composer_(composer),
		buf_(pp.max_buf),
		pp_(pp),
		last_batch_(false),
		last_batch_size_(0) { }
	
	/**
	 * Use objects in the PatternSource and/or PatternComposer
	 * hierarchies to populate the per-thread buffers.
	 */
	std::pair<bool, bool> nextReadPair();
	
	Read& read_a() { return buf_.read_a(); }
	Read& read_b() { return buf_.read_b(); }
	
	const Read& read_a() const { return buf_.read_a(); }
	const Read& read_b() const { return buf_.read_b(); }
	
private:
	
	/**
	 * When we've finished fully parsing and dishing out reads in
	 * the current batch, we go get the next one by calling into
	 * the composition layer.
	 */
	std::pair<bool, int> nextBatch() {
		buf_.reset();
		std::pair<bool, int> res = composer_.nextBatch(buf_);
		buf_.init();
		return res;
	}
	
	/**
	 * Once name/sequence/qualities have been parsed for an
	 * unpaired read, set all the other key fields of the Read
	 * struct.
	 */
	void finalize(Read& ra);

	/**
	 * Once name/sequence/qualities have been parsed for a
	 * paired-end read, set all the other key fields of the Read
	 * structs.
	 */
	void finalizePair(Read& ra, Read& rb);
	
	/**
	 * Call into composition layer (which in turn calls into
	 * format layer) to parse the read.
	 */
	bool parse(Read& ra, Read& rb) {
		return composer_.parse(ra, rb, buf_.rdid());
	}

	PatternComposer& composer_; // pattern composer
	PerThreadReadBuf buf_;		// read data buffer
	const PatternParams& pp_;	// pattern-related parameters
	bool last_batch_;			// true if this is final batch
	int last_batch_size_;		// # reads read in previous batch
};

/**
 * Abstract parent factory for PatternSourcePerThreads.
 */
class PatternSourcePerThreadFactory {
public:
	PatternSourcePerThreadFactory(
		PatternComposer& composer,
		const PatternParams& pp) :
		composer_(composer),
		pp_(pp) { }
	
	/**
	 * Create a new heap-allocated PatternSourcePerThreads.
	 */
	virtual PatternSourcePerThread* create() const {
		return new PatternSourcePerThread(composer_, pp_);
	}
	
	/**
	 * Create a new heap-allocated vector of heap-allocated
	 * PatternSourcePerThreads.
	 */
	virtual EList<PatternSourcePerThread*>* create(uint32_t n) const {
		EList<PatternSourcePerThread*>* v = new EList<PatternSourcePerThread*>;
		for(size_t i = 0; i < n; i++) {
			v->push_back(new PatternSourcePerThread(composer_, pp_));
			assert(v->back() != NULL);
		}
		return v;
	}
	
private:
	/// Container for obtaining paired reads from PatternSources
	PatternComposer& composer_;
	const PatternParams& pp_;
};

#endif /*PAT_H_*/
