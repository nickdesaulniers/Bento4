/*****************************************************************
|
|    AP4 - MP4 to HLS File Converter
|
|    Copyright 2002-2015 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
|    Bento4|GPL is distributed in the hope that it will be useful,
|    but WITHOUT ANY WARRANTY; without even the implied warranty of
|    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
|    GNU General Public License for more details.
|
|    You should have received a copy of the GNU General Public License
|    along with Bento4|GPL; see the file COPYING.  If not, write to the
|    Free Software Foundation, 59 Temple Place - Suite 330, Boston, MA
|    02111-1307, USA.
|
 ****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>

#include "Ap4.h"
#include "Ap4StreamCipher.h"
#include "Ap4Mp4AudioInfo.h"

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
#define BANNER "MP4 To HLS File Converter - Version 1.1\n"\
               "(Bento4 Version " AP4_VERSION_STRING ")\n"\
               "(c) 2002-2015 Axiomatic Systems, LLC"
 
/*----------------------------------------------------------------------
|   options
+---------------------------------------------------------------------*/
typedef enum {
    ENCRYPTION_MODE_NONE,
    ENCRYPTION_MODE_AES_128,
    ENCRYPTION_MODE_SAMPLE_AES
} EncryptionMode;

typedef enum {
    ENCRYPTION_IV_MODE_NONE,
    ENCRYPTION_IV_MODE_SEQUENCE,
    ENCRYPTION_IV_MODE_RANDOM,
    ENCRYPTION_IV_MODE_FPS
} EncryptionIvMode;

struct _Options {
    const char*      input;
    bool             verbose;
    unsigned int     hls_version;
    unsigned int     pmt_pid;
    unsigned int     audio_pid;
    unsigned int     video_pid;
    const char*      index_filename;
    bool             output_single_file;
    const char*      segment_filename_template;
    const char*      segment_url_template;
    unsigned int     segment_duration;
    unsigned int     segment_duration_threshold;
    const char*      encryption_key_hex;
    AP4_UI08         encryption_key[16];
    AP4_UI08         encryption_iv[16];
    EncryptionMode   encryption_mode;
    EncryptionIvMode encryption_iv_mode;
    const char*      encryption_key_uri;
    const char*      encryption_key_format;
    const char*      encryption_key_format_versions;
} Options;

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
static const unsigned int DefaultSegmentDurationThreshold = 50; // milliseconds

const AP4_UI08 AP4_MPEG2_STREAM_TYPE_SAMPLE_AES_AVC             = 0xDB;
const AP4_UI08 AP4_MPEG2_STREAM_TYPE_SAMPLE_AES_ISO_IEC_13818_7 = 0xCF;
const AP4_UI08 AP4_MPEG2_STREAM_TYPE_SAMPLE_AES_ATSC_AC3        = 0xC1;
const AP4_UI08 AP4_MPEG2_PRIVATE_DATA_INDICATOR_DESCRIPTOR_TAG  = 15;
const AP4_UI08 AP4_MPEG2_REGISTRATION_DESCRIPTOR_TAG            = 5;

/*----------------------------------------------------------------------
|   PrintUsageAndExit
+---------------------------------------------------------------------*/
static void
PrintUsageAndExit()
{
    fprintf(stderr, 
            BANNER 
            "\n\nusage: mp42hls [options] <input>\n"
            "Options:\n"
            "  --verbose\n"
            "  --hls-version <n> (default: 3)\n"
            "  --pmt-pid <pid>\n"
            "    PID to use for the PMT (default: 0x100)\n"
            "  --audio-pid <pid>\n"
            "    PID to use for audio packets (default: 0x101)\n"
            "  --video-pid <pid>\n"
            "    PID to use for video packets (default: 0x102)\n"
            "  --segment-duration <n>\n"
            "    Target segment duration in seconds (default: 10)\n"
            "  --segment-duration-threshold <t>\n"
            "    Segment duration threshold in milliseconds (default: 50)\n"
            "  --index-filename <filename>\n"
            "    Filename to use for the playlist/index (default: stream.m3u8)\n"
            "  --segment-filename-template <pattern>\n"
            "    Filename pattern to use for the segments. Use a printf-style pattern with\n"
            "    one number field for the segment number, unless using single file mode\n"
            "    (default: segment-%%d.ts for separate segment files, or stream.ts for single file)\n"
            "  --segment-url-template <pattern>\n"
            "    URL pattern to use for the segments. Use a printf-style pattern with\n"
            "    one number field for the segment number unless unsing single file mode.\n"
            "    (may be a relative or absolute URI).\n"
            "    (default: segment-%%d.ts for separate segment files, or stream.ts for single file)\n"
            "  --output-single-file\n"
            "    Output all the media in a single file instead of separate segment files.\n"
            "    The segment filename template and segment URL template must be simple strings\n"
            "    without '%%d' or other printf-style patterns\n"
            "  --encryption-mode <mode>\n"
            "    Encryption mode (only used when --encryption-key is specified). AES-128 or SAMPLE-AES (default: AES-128)\n"
            "  --encryption-key <key>\n"
            "    Encryption key in hexadecimal (default: no encryption)\n"
            "  --encryption-iv-mode <mode>\n"
            "    Encryption IV mode: 'sequence', 'random' or 'fps' (Fairplay Streaming) (default: sequence)\n"
            "    (when the mode is 'fps', the encryption key must be 32 bytes: 16 bytes for the key\n"
            "    followed by 16 bytes for the IV).\n"
            "  --encryption-key-uri <uri>\n"
            "    Encryption key URI (may be a realtive or absolute URI). (default: key.bin)\n"
            "  --encryption-key-format <format>\n"
            "    Encryption key format. (default: 'identity')\n"
            "  --encryption-key-format-versions <versions>\n"
            "    Encryption key format versions.\n"
            );
    exit(1);
}

/*----------------------------------------------------------------------
|   SampleReader
+---------------------------------------------------------------------*/
class SampleReader 
{
public:
    virtual ~SampleReader() {}
    virtual AP4_Result ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data) = 0;
};

/*----------------------------------------------------------------------
|   TrackSampleReader
+---------------------------------------------------------------------*/
class TrackSampleReader : public SampleReader
{
public:
    TrackSampleReader(AP4_Track& track) : m_Track(track), m_SampleIndex(0) {}
    AP4_Result ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data);
    
private:
    AP4_Track&  m_Track;
    AP4_Ordinal m_SampleIndex;
};

/*----------------------------------------------------------------------
|   TrackSampleReader
+---------------------------------------------------------------------*/
AP4_Result 
TrackSampleReader::ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data)
{
    if (m_SampleIndex >= m_Track.GetSampleCount()) return AP4_ERROR_EOS;
    return m_Track.ReadSample(m_SampleIndex++, sample, sample_data);
}

/*----------------------------------------------------------------------
|   FragmentedSampleReader
+---------------------------------------------------------------------*/
class FragmentedSampleReader : public SampleReader 
{
public:
    FragmentedSampleReader(AP4_LinearReader& fragment_reader, AP4_UI32 track_id) :
        m_FragmentReader(fragment_reader), m_TrackId(track_id) {
        fragment_reader.EnableTrack(track_id);
    }
    AP4_Result ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data);
    
private:
    AP4_LinearReader& m_FragmentReader;
    AP4_UI32          m_TrackId;
};

/*----------------------------------------------------------------------
|   FragmentedSampleReader
+---------------------------------------------------------------------*/
AP4_Result 
FragmentedSampleReader::ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data)
{
    return m_FragmentReader.ReadNextSample(m_TrackId, sample, sample_data);
}

/*----------------------------------------------------------------------
|   OpenOutput
+---------------------------------------------------------------------*/
static AP4_ByteStream*
OpenOutput(const char* filename_pattern, unsigned int segment_number)
{
    AP4_ByteStream* output = NULL;
    char filename[4096];
    sprintf(filename, filename_pattern, segment_number);
    AP4_Result result = AP4_FileByteStream::Create(filename, AP4_FileByteStream::STREAM_MODE_WRITE, output);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot open output (%d)\n", result);
        return NULL;
    }
    
    return output;
}

/*----------------------------------------------------------------------
|   PreventStartCodeEmulation
+---------------------------------------------------------------------*/
static void
PreventStartCodeEmulation(const AP4_UI08* payload, AP4_Size payload_size, AP4_DataBuffer& output)
{
    output.Reserve(payload_size*2); // more than enough
    AP4_Size  output_size = 0;
    AP4_UI08* buffer = output.UseData();
    
	unsigned int zero_counter = 0;
	for (unsigned int i = 0; i < payload_size; i++) {
		if (zero_counter == 2) {
            if (payload[i] == 0 || payload[i] == 1 || payload[i] == 2 || payload[i] == 3) {
                buffer[output_size++] = 3;
                zero_counter = 0;
            }
		}

        buffer[output_size++] = payload[i];

		if (payload[i] == 0) {
			++zero_counter;
		} else {
			zero_counter = 0;
		}		
	}

    output.SetDataSize(output_size);
}

/*----------------------------------------------------------------------
|   EncryptingStream
+---------------------------------------------------------------------*/
class EncryptingStream: public AP4_ByteStream {
public:
    static AP4_Result Create(const AP4_UI08* key, const AP4_UI08* iv, AP4_ByteStream* output, EncryptingStream*& stream);
    virtual AP4_Result ReadPartial(void* , AP4_Size, AP4_Size&) {
        return AP4_ERROR_NOT_SUPPORTED;
    }
    virtual AP4_Result WritePartial(const void* buffer,
                                    AP4_Size    bytes_to_write, 
                                    AP4_Size&   bytes_written) {
        AP4_UI08* out = new AP4_UI08[bytes_to_write+16];
        AP4_Size  out_size = bytes_to_write+16;
        AP4_Result result = m_StreamCipher->ProcessBuffer((const AP4_UI08*)buffer,
                                                          bytes_to_write,
                                                          out,
                                                          &out_size);
        if (AP4_SUCCEEDED(result)) {
            result = m_Output->Write(out, out_size);
            bytes_written = bytes_to_write;
            m_Size     += out_size;
        } else {
            bytes_written = 0;
        }
        delete[] out;
        return result;
    }
    virtual AP4_Result Flush() {
        AP4_UI08 trailer[16];
        AP4_Size trailer_size = sizeof(trailer);
        AP4_Result result = m_StreamCipher->ProcessBuffer(NULL, 0, trailer, &trailer_size, true);
        if (AP4_SUCCEEDED(result) && trailer_size) {
            m_Output->Write(trailer, trailer_size);
            m_Size += trailer_size;
        }
        
        return AP4_SUCCESS;
    }
    virtual AP4_Result Seek(AP4_Position) { return AP4_ERROR_NOT_SUPPORTED; }
    virtual AP4_Result Tell(AP4_Position& position) { position = m_Size; return AP4_SUCCESS; }
    virtual AP4_Result GetSize(AP4_LargeSize& size) { size = m_Size;     return AP4_SUCCESS; }
    
    void AddReference() {
        ++m_ReferenceCount;
    }
    void Release() {
        if (--m_ReferenceCount == 0) {
            delete this;
        }
    }

private:
    EncryptingStream(AP4_CbcStreamCipher* stream_cipher, AP4_ByteStream* output):
        m_ReferenceCount(1),
        m_StreamCipher(stream_cipher),
        m_Output(output),
        m_Size(0) {
        output->AddReference();
    }
    ~EncryptingStream() {
        m_Output->Release();
        delete m_StreamCipher;
    }
    unsigned int         m_ReferenceCount;
    AP4_CbcStreamCipher* m_StreamCipher;
    AP4_ByteStream*      m_Output;
    AP4_LargeSize        m_Size;
};

/*----------------------------------------------------------------------
|   EncryptingStream::Create
+---------------------------------------------------------------------*/
AP4_Result
EncryptingStream::Create(const AP4_UI08* key, const AP4_UI08* iv, AP4_ByteStream* output, EncryptingStream*& stream) {
    stream = NULL;
    AP4_BlockCipher* block_cipher = NULL;
    AP4_Result result = AP4_DefaultBlockCipherFactory::Instance.CreateCipher(AP4_BlockCipher::AES_128,
                                                                             AP4_BlockCipher::ENCRYPT,
                                                                             AP4_BlockCipher::CBC,
                                                                             NULL,
                                                                             key,
                                                                             16,
                                                                             block_cipher);
    if (AP4_FAILED(result)) return result;
    AP4_CbcStreamCipher* stream_cipher = new AP4_CbcStreamCipher(block_cipher);
    stream_cipher->SetIV(iv);
    stream = new EncryptingStream(stream_cipher, output);
    
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   SampleEncrypter
+---------------------------------------------------------------------*/
class SampleEncrypter {
public:
    static AP4_Result Create(const AP4_UI08* key, const AP4_UI08* iv, SampleEncrypter*& encrypter);
    ~SampleEncrypter() {
        delete m_StreamCipher;
    }

    void EncryptAudioSample(AP4_DataBuffer& sample);
    void EncryptVideoSample(AP4_DataBuffer& sample, AP4_UI08 nalu_length_size);
    
private:
    SampleEncrypter(AP4_CbcStreamCipher* stream_cipher, const AP4_UI08* iv):
        m_StreamCipher(stream_cipher) {
        AP4_CopyMemory(m_IV, iv, 16);
    }

    AP4_CbcStreamCipher* m_StreamCipher;
    AP4_UI08             m_IV[16];
};

/*----------------------------------------------------------------------
|   SampleEncrypter::Create
+---------------------------------------------------------------------*/
AP4_Result
SampleEncrypter::Create(const AP4_UI08* key, const AP4_UI08* iv, SampleEncrypter*& encrypter) {
    encrypter = NULL;
    AP4_BlockCipher* block_cipher = NULL;
    AP4_Result result = AP4_DefaultBlockCipherFactory::Instance.CreateCipher(AP4_BlockCipher::AES_128,
                                                                             AP4_BlockCipher::ENCRYPT,
                                                                             AP4_BlockCipher::CBC,
                                                                             NULL,
                                                                             key,
                                                                             16,
                                                                             block_cipher);
    if (AP4_FAILED(result)) return result;
    AP4_CbcStreamCipher* stream_cipher = new AP4_CbcStreamCipher(block_cipher);
    encrypter = new SampleEncrypter(stream_cipher, iv);
    
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   SampleEncrypter::EncryptAudioSample
|
|   [ADTS header if AAC]
|   unencrypted_leader                 // 16 bytes
|   while (bytes_remaining() >= 16) {
|       protected_block                // 16 bytes
|   }
|   unencrypted_trailer                // 0-15 bytes
+---------------------------------------------------------------------*/
void
SampleEncrypter::EncryptAudioSample(AP4_DataBuffer& sample)
{
    if (sample.GetDataSize() <= 16) {
        return;
    }
    unsigned int encrypted_block_count = (sample.GetDataSize()-16)/16;
    AP4_Size encrypted_size = encrypted_block_count*16;
    m_StreamCipher->SetIV(m_IV);
    m_StreamCipher->ProcessBuffer(sample.UseData()+16, encrypted_size, sample.UseData()+16, &encrypted_size);
}

/*----------------------------------------------------------------------
|   SampleEncrypter::EncryptVideoSample
|
|   Sequence of NAL Units:
|   NAL_unit_type_byte                // 1 byte
|   unencrypted_leader                // 31 bytes
|   while (bytes_remaining() > 16) {
|       protected_block_one_in_ten    // 16 bytes
|   }
|   unencrypted_trailer               // 1-16 bytes
+---------------------------------------------------------------------*/
void
SampleEncrypter::EncryptVideoSample(AP4_DataBuffer& sample, AP4_UI08 nalu_length_size)
{
    AP4_DataBuffer encrypted;
    
    AP4_UI08* nalu = sample.UseData();
    AP4_Size  bytes_remaining = sample.GetDataSize();
    while (bytes_remaining > nalu_length_size) {
        AP4_Size nalu_length = 0;
        switch (nalu_length_size) {
            case 1:
                nalu_length = nalu[0];
                break;
                
            case 2:
                nalu_length = AP4_BytesToUInt16BE(nalu);
                break;
    
            case 4:
                nalu_length = AP4_BytesToUInt32BE(nalu);
                break;
                
            default:
                break;
        }
        
        if (bytes_remaining < nalu_length_size+nalu_length) {
            break;
        }
        
        AP4_UI08 nalu_type = nalu[nalu_length_size] & 0x1F;
        if (nalu_length > 48 && (nalu_type == 1 || nalu_type == 5)) {
            AP4_Size encrypted_size = 16*((nalu_length-32)/16);
            if ((nalu_length%16) == 0) {
                encrypted_size -= 16;
            }
            m_StreamCipher->SetIV(m_IV);
            for (unsigned int i=0; i<encrypted_size; i += 10*16) {
                AP4_Size one_block_size = 16;
                m_StreamCipher->ProcessBuffer(nalu+nalu_length_size+32+i, one_block_size,
                                              nalu+nalu_length_size+32+i, &one_block_size);
            }

            // perform startcode emulation prevention
            AP4_DataBuffer escaped_nalu;
            PreventStartCodeEmulation(nalu+nalu_length_size, nalu_length, escaped_nalu);
            
            // the size may have changed
            // FIXME: this could overflow if nalu_length_size is too small
            switch (nalu_length_size) {
                case 1:
                    nalu[0] = (AP4_UI08)(escaped_nalu.GetDataSize()&0xFF);
                    break;
                    
                case 2:
                    AP4_BytesFromUInt16BE(nalu, escaped_nalu.GetDataSize());
                    break;
        
                case 4:
                    AP4_BytesFromUInt32BE(nalu, escaped_nalu.GetDataSize());
                    break;
                    
                default:
                    break;
            }

            encrypted.AppendData(nalu, nalu_length_size);
            encrypted.AppendData(escaped_nalu.GetData(), escaped_nalu.GetDataSize());
        } else {
            encrypted.AppendData(nalu, nalu_length_size);
            encrypted.AppendData(nalu+nalu_length_size, nalu_length);
        }
        
        nalu            += nalu_length_size+nalu_length;
        bytes_remaining -= nalu_length_size+nalu_length;
    }
    
    sample.SetData(encrypted.GetData(), encrypted.GetDataSize());
}

/*----------------------------------------------------------------------
|   ReadSample
+---------------------------------------------------------------------*/
static AP4_Result
ReadSample(SampleReader&   reader, 
           AP4_Track&      track,
           AP4_Sample&     sample,
           AP4_DataBuffer& sample_data, 
           double&         ts,
           bool&           eos)
{
    AP4_Result result = reader.ReadSample(sample, sample_data);
    if (AP4_FAILED(result)) {
        if (result == AP4_ERROR_EOS) {
            eos = true;
        } else {
            return result;
        }
    }
    ts = (double)sample.GetDts()/(double)track.GetMediaTimeScale();
    
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   WriteSamples
+---------------------------------------------------------------------*/
static AP4_Result
WriteSamples(AP4_Mpeg2TsWriter&               writer,
             AP4_Track*                       audio_track,
             SampleReader*                    audio_reader, 
             AP4_Mpeg2TsWriter::SampleStream* audio_stream,
             AP4_Track*                       video_track,
             SampleReader*                    video_reader, 
             AP4_Mpeg2TsWriter::SampleStream* video_stream,
             unsigned int                     segment_duration_threshold,
             AP4_UI08                         nalu_length_size)
{
    AP4_Sample          audio_sample;
    AP4_DataBuffer      audio_sample_data;
    unsigned int        audio_sample_count = 0;
    double              audio_ts = 0.0;
    bool                audio_eos = false;
    AP4_Sample          video_sample;
    AP4_DataBuffer      video_sample_data;
    unsigned int        video_sample_count = 0;
    double              video_ts = 0.0;
    bool                video_eos = false;
    double              last_ts = 0.0;
    AP4_Position        segment_start = 0;
    unsigned int        segment_number = 0;
    double              segment_duration = 0.0;
    AP4_ByteStream*     output = NULL;
    AP4_ByteStream*     raw_output = NULL;
    AP4_ByteStream*     playlist = NULL;
    char                string_buffer[4096];
    AP4_Array<double>   segment_durations;
    AP4_Array<AP4_UI32> segment_sizes;
    bool                new_segment = true;
    SampleEncrypter*    sample_encrypter = NULL;
    AP4_Result          result = AP4_SUCCESS;
    
    // prime the samples
    if (audio_reader) {
        result = ReadSample(*audio_reader, *audio_track, audio_sample, audio_sample_data, audio_ts, audio_eos);
        if (AP4_FAILED(result)) return result;
    }
    if (video_reader) {
        result = ReadSample(*video_reader, *video_track, video_sample, video_sample_data, video_ts, video_eos);
        if (AP4_FAILED(result)) return result;
    }
    
    for (;;) {
        bool sync_sample = false;
        AP4_Track* chosen_track= NULL;
        if (audio_track && !audio_eos) {
            chosen_track = audio_track;
            if (video_track == NULL) sync_sample = true;
        }
        if (video_track && !video_eos) {
            if (audio_track) {
                if (video_ts <= audio_ts) {
                    chosen_track = video_track;
                }
            } else {
                chosen_track = video_track;
            }
            if (chosen_track == video_track && video_sample.IsSync()) {
                sync_sample = true;
            }
        }
        if (chosen_track == NULL) break;
        
        // check if we need to start a new segment
        if (Options.segment_duration && sync_sample) {
            if (video_track) {
                segment_duration = video_ts - last_ts;
            } else {
                segment_duration = audio_ts - last_ts;
            }
            if (segment_duration >= (double)Options.segment_duration - (double)segment_duration_threshold/1000.0) {
                if (video_track) {
                    last_ts = video_ts;
                } else {
                    last_ts = audio_ts;
                }
                if (output) {
                    output->Flush();
                    AP4_UI32 segment_size = 0;
                    if (Options.encryption_mode == ENCRYPTION_MODE_AES_128) {
                        AP4_LargeSize segment_end = 0;
                        output->GetSize(segment_end);
                        segment_size = (AP4_UI32)(segment_end-segment_start);
                    } else {
                        AP4_Position segment_end = 0;
                        output->Tell(segment_end);
                        segment_size = (AP4_UI32)(segment_end-segment_start);
                    }
                    segment_sizes.Append(segment_size);
                    segment_durations.Append(segment_duration);
                    if (Options.verbose) {
                        printf("Segment %d, duration=%.2f, %d audio samples, %d video samples, %d bytes\n",
                               segment_number, 
                               segment_duration,
                               audio_sample_count, 
                               video_sample_count,
                               segment_size);
                    }
                    if (Options.output_single_file) {
                        if (Options.encryption_mode == ENCRYPTION_MODE_AES_128) {
                            segment_start = 0;
                        } else {
                            output->Tell(segment_start);
                        }
                    } else {
                        output->Release();
                        output = NULL;
                        segment_start = 0;
                    }
                    ++segment_number;
                    audio_sample_count = 0;
                    video_sample_count = 0;
                }
                new_segment = true;
            }
        }
        if (new_segment) {
            new_segment = false;
            if (output == NULL) {
                output = OpenOutput(Options.segment_filename_template, segment_number);
                raw_output = output;
                if (output == NULL) return AP4_ERROR_CANNOT_OPEN_FILE;
            }
            if (Options.encryption_mode != ENCRYPTION_MODE_NONE) {
                if (Options.encryption_iv_mode == ENCRYPTION_IV_MODE_SEQUENCE) {
                    AP4_SetMemory(Options.encryption_iv, 0, sizeof(Options.encryption_iv));
                    AP4_BytesFromUInt32BE(&Options.encryption_iv[12], segment_number);
                }
            }
            if (Options.encryption_mode == ENCRYPTION_MODE_AES_128) {
                EncryptingStream* encrypting_stream = NULL;
                result = EncryptingStream::Create(Options.encryption_key, Options.encryption_iv, raw_output, encrypting_stream);
                if (AP4_FAILED(result)) {
                    fprintf(stderr, "ERROR: failed to create encrypting stream (%d)\n", result);
                    return 1;
                }
                output->Release();
                output = encrypting_stream;
            } else if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES) {
                delete sample_encrypter;
                sample_encrypter = NULL;
                result = SampleEncrypter::Create(Options.encryption_key, Options.encryption_iv, sample_encrypter);
                if (AP4_FAILED(result)) {
                    fprintf(stderr, "ERROR: failed to create sample encrypter (%d)\n", result);
                    return 1;
                }
            }
            writer.WritePAT(*output);
            writer.WritePMT(*output);
        }

        // write the samples out and advance to the next sample
        if (chosen_track == audio_track) {
            // perform sample-level encryption if needed
            if (sample_encrypter) {
                sample_encrypter->EncryptAudioSample(audio_sample_data);
            }
            
            // write the sample data
            result = audio_stream->WriteSample(audio_sample, 
                                               audio_sample_data,
                                               audio_track->GetSampleDescription(audio_sample.GetDescriptionIndex()), 
                                               video_track==NULL, 
                                               *output);
            if (AP4_FAILED(result)) return result;
            
            result = ReadSample(*audio_reader, *audio_track, audio_sample, audio_sample_data, audio_ts, audio_eos);
            if (AP4_FAILED(result)) return result;
            ++audio_sample_count;
        } else if (chosen_track == video_track) {
            // perform sample-level encryption if needed
            if (sample_encrypter) {
                sample_encrypter->EncryptVideoSample(video_sample_data, nalu_length_size);
            }

            // write the sample data
            result = video_stream->WriteSample(video_sample,
                                               video_sample_data, 
                                               video_track->GetSampleDescription(video_sample.GetDescriptionIndex()),
                                               true, 
                                               *output);
            if (AP4_FAILED(result)) return result;

            result = ReadSample(*video_reader, *video_track, video_sample, video_sample_data, video_ts, video_eos);
            if (AP4_FAILED(result)) return result;
            ++video_sample_count;
        } else {
            break;
        }
    }
    
    // finish the last segment
    if (output) {
        if (video_track) {
            segment_duration = video_ts - last_ts;
        } else {
            segment_duration = audio_ts - last_ts;
        }
        output->Flush();
        AP4_UI32 segment_size = 0;
        if (Options.encryption_mode == ENCRYPTION_MODE_AES_128) {
            AP4_LargeSize segment_end = 0;
            output->GetSize(segment_end);
            segment_size = (AP4_UI32)(segment_end-segment_start);
        } else {
            AP4_Position segment_end = 0;
            output->Tell(segment_end);
            segment_size = (AP4_UI32)(segment_end-segment_start);
        }
        segment_sizes.Append(segment_size);
        segment_durations.Append(segment_duration);
        if (Options.verbose) {
            printf("Segment %d, duration=%.2f, %d audio samples, %d video samples, %d bytes\n",
                   segment_number, 
                   segment_duration,
                   audio_sample_count, 
                   video_sample_count,
                   segment_size);
        }
        output->Release();
        output = NULL;
        ++segment_number;
        audio_sample_count = 0;
        video_sample_count = 0;
    }

    // create the playlist/index file
    playlist = OpenOutput(Options.index_filename, 0);
    if (playlist == NULL) return AP4_ERROR_CANNOT_OPEN_FILE;

    unsigned int target_duration = 0;
    for (unsigned int i=0; i<segment_durations.ItemCount(); i++) {
        if ((unsigned int)(segment_durations[i]+0.5) > target_duration) {
            target_duration = segment_durations[i];
        }
    }

    playlist->WriteString("#EXTM3U\r\n");
    if (Options.hls_version > 1) {
        sprintf(string_buffer, "#EXT-X-VERSION:%d\r\n", Options.hls_version);
        playlist->WriteString(string_buffer);
    }
    playlist->WriteString("#EXT-X-PLAYLIST-TYPE:VOD\r\n");
    playlist->WriteString("#EXT-X-INDEPENDENT-SEGMENTS\r\n");
    playlist->WriteString("#EXT-X-TARGETDURATION:");
    sprintf(string_buffer, "%d\r\n", target_duration);
    playlist->WriteString(string_buffer);
    playlist->WriteString("#EXT-X-MEDIA-SEQUENCE:0\r\n");

    if (Options.encryption_mode != ENCRYPTION_MODE_NONE) {
        playlist->WriteString("#EXT-X-KEY:METHOD=");
        if (Options.encryption_mode == ENCRYPTION_MODE_AES_128) {
            playlist->WriteString("AES-128");
        } else if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES) {
            playlist->WriteString("SAMPLE-AES");
        }
        playlist->WriteString(",URI=\"");
        playlist->WriteString(Options.encryption_key_uri);
        playlist->WriteString("\"");
        if (Options.encryption_iv_mode == ENCRYPTION_IV_MODE_RANDOM) {
            playlist->WriteString(",IV=0x");
            char iv_hex[33];
            iv_hex[32] = 0;
            AP4_FormatHex(Options.encryption_iv, 16, iv_hex);
            playlist->WriteString(iv_hex);
        }
        if (Options.encryption_key_format) {
            playlist->WriteString(",KEYFORMAT=\"");
            playlist->WriteString(Options.encryption_key_format);
            playlist->WriteString("\"");
        }
        if (Options.encryption_key_format_versions) {
            playlist->WriteString(",KEYFORMATVERSIONS=\"");
            playlist->WriteString(Options.encryption_key_format_versions);
            playlist->WriteString("\"");
        }
        playlist->WriteString("\r\n");
    }
    
    AP4_UI64 segment_position = 0;
    for (unsigned int i=0; i<segment_durations.ItemCount(); i++) {
        if (Options.hls_version >= 3) {
            sprintf(string_buffer, "#EXTINF:%f,\r\n", segment_durations[i]);
        } else {
            sprintf(string_buffer, "#EXTINF:%u,\r\n", (unsigned int)(segment_durations[i]+0.5));
        }
        playlist->WriteString(string_buffer);
        if (Options.output_single_file) {
            sprintf(string_buffer, "#EXT-X-BYTERANGE:%d@%lld\r\n", segment_sizes[i], segment_position);
            segment_position += segment_sizes[i];
            playlist->WriteString(string_buffer);
        }
        sprintf(string_buffer, Options.segment_url_template, i);
        playlist->WriteString(string_buffer);
        playlist->WriteString("\r\n");
    }
                    
    playlist->WriteString("#EXT-X-ENDLIST\r\n");
    playlist->Release();

    if (Options.verbose) {
        if (video_track) {
            segment_duration = video_ts - last_ts;
        } else {
            segment_duration = audio_ts - last_ts;
        }
        printf("Conversion complete, duration=%.2f secs\n", segment_duration);
    }
    
    if (output) output->Release();
    delete sample_encrypter;
    
    return result;
}

/*----------------------------------------------------------------------
|   main
+---------------------------------------------------------------------*/
int
main(int argc, char** argv)
{
    if (argc < 2) {
        PrintUsageAndExit();
    }
    
    // default options
    Options.input                          = NULL;
    Options.verbose                        = false;
    Options.hls_version                    = 0;
    Options.pmt_pid                        = 0x100;
    Options.audio_pid                      = 0x101;
    Options.video_pid                      = 0x102;
    Options.output_single_file             = false;
    Options.index_filename                 = "stream.m3u8";
    Options.segment_filename_template      = NULL;
    Options.segment_url_template           = NULL;
    Options.segment_duration               = 10;
    Options.segment_duration_threshold     = DefaultSegmentDurationThreshold;
    Options.encryption_key_hex             = NULL;
    Options.encryption_mode                = ENCRYPTION_MODE_NONE;
    Options.encryption_iv_mode             = ENCRYPTION_IV_MODE_NONE;
    Options.encryption_key_uri             = "key.bin";
    Options.encryption_key_format          = NULL;
    Options.encryption_key_format_versions = NULL;
    AP4_SetMemory(Options.encryption_key, 0, sizeof(Options.encryption_key));
    AP4_SetMemory(Options.encryption_iv,  0, sizeof(Options.encryption_iv));
    
    // parse command line
    AP4_Result result;
    char** args = argv+1;
    while (const char* arg = *args++) {
        if (!strcmp(arg, "--verbose")) {
            Options.verbose = true;
        } else if (!strcmp(arg, "--hls-version")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --hls-version requires a number\n");
                return 1;
            }
            Options.hls_version = strtoul(*args++, NULL, 10);
            if (Options.hls_version ==0) {
                fprintf(stderr, "ERROR: --hls-version requires number > 0\n");
                return 1;
            }
        } else if (!strcmp(arg, "--segment-duration")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --segment-duration requires a number\n");
                return 1;
            }
            Options.segment_duration = strtoul(*args++, NULL, 10);
        } else if (!strcmp(arg, "--segment-duration-threshold")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --segment-duration-threshold requires a number\n");
                return 1;
            }
            Options.segment_duration_threshold = strtoul(*args++, NULL, 10);
        } else if (!strcmp(arg, "--segment-filename-template")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --segment-filename-template requires an argument\n");
                return 1;
            }
            Options.segment_filename_template = *args++;
        } else if (!strcmp(arg, "--segment-url-template")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --segment-url-template requires an argument\n");
                return 1;
            }
            Options.segment_url_template = *args++;
        } else if (!strcmp(arg, "--pmt-pid")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --pmt-pid requires a number\n");
                return 1;
            }
            Options.pmt_pid = strtoul(*args++, NULL, 10);
        } else if (!strcmp(arg, "--audio-pid")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --audio-pid requires a number\n");
                return 1;
            }
            Options.audio_pid = strtoul(*args++, NULL, 10);
        } else if (!strcmp(arg, "--video-pid")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --video-pid requires a number\n");
                return 1;
            }
            Options.video_pid = strtoul(*args++, NULL, 10);
        } else if (!strcmp(arg, "--output-single-file")) {
            Options.output_single_file = true;
        } else if (!strcmp(arg, "--index-filename")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --index-filename requires a filename\n");
                return 1;
            }
            Options.index_filename = *args++;
        } else if (!strcmp(arg, "--encryption-key")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --encryption-key requires an argument\n");
                return 1;
            }
            Options.encryption_key_hex = *args++;
            result = AP4_ParseHex(Options.encryption_key_hex, Options.encryption_key, 16);
            if (AP4_FAILED(result)) {
                fprintf(stderr, "ERROR: invalid hex key\n");
                return 1;
            }
            if (Options.encryption_mode == ENCRYPTION_MODE_NONE) {
                Options.encryption_mode = ENCRYPTION_MODE_AES_128;
            }
        } else if (!strcmp(arg, "--encryption-mode")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --encryption-mode requires an argument\n");
                return 1;
            }
            if (strncmp(*args, "AES-128", 7) == 0) {
                Options.encryption_mode = ENCRYPTION_MODE_AES_128;
            } else if (strncmp(*args, "SAMPLE-AES", 10) == 0) {
                Options.encryption_mode = ENCRYPTION_MODE_SAMPLE_AES;
            } else {
                fprintf(stderr, "ERROR: unknown encryption mode\n");
                return 1;
            }
            ++args;
        } else if (!strcmp(arg, "--encryption-iv-mode")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --encryption-iv-mode requires an argument\n");
                return 1;
            }
            if (strncmp(*args, "sequence", 8) == 0) {
                Options.encryption_iv_mode = ENCRYPTION_IV_MODE_SEQUENCE;
            } else if (strncmp(*args, "random", 6) == 0) {
                Options.encryption_iv_mode = ENCRYPTION_IV_MODE_RANDOM;
            } else if (strncmp(*args, "fps", 6) == 0) {
                Options.encryption_iv_mode = ENCRYPTION_IV_MODE_FPS;
            } else {
                fprintf(stderr, "ERROR: unknown encryption IV mode\n");
                return 1;
            }
            ++args;
        } else if (!strcmp(arg, "--encryption-key-uri")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --encryption-key-uri requires an argument\n");
                return 1;
            }
            Options.encryption_key_uri = *args++;
        } else if (!strcmp(arg, "--encryption-key-format")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --encryption-key-format requires an argument\n");
                return 1;
            }
            Options.encryption_key_format = *args++;
        } else if (!strcmp(arg, "--encryption-key-format-versions")) {
            if (*args == NULL) {
                fprintf(stderr, "ERROR: --encryption-key-format-versions requires an argument\n");
                return 1;
            }
            Options.encryption_key_format_versions = *args++;
        } else if (Options.input == NULL) {
            Options.input = arg;
        } else {
            fprintf(stderr, "ERROR: unexpected argument: %s\n", arg);
            return 1;
        }
    }

    // check args
    if (Options.input == NULL) {
        fprintf(stderr, "ERROR: missing input file name\n");
        return 1;
    }
    if (Options.encryption_mode != ENCRYPTION_MODE_NONE && Options.encryption_key_hex == NULL) {
        fprintf(stderr, "ERROR: no encryption key specified\n");
        return 1;
    }
    if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES && Options.hls_version > 0 && Options.hls_version < 5) {
        Options.hls_version = 5;
        fprintf(stderr, "WARNING: forcing version to 5 in order to support SAMPLE-AES encryption\n");
    }
    if (Options.encryption_iv_mode == ENCRYPTION_IV_MODE_NONE) {
        Options.encryption_iv_mode = ENCRYPTION_IV_MODE_SEQUENCE;
    }
    if ((Options.encryption_key_format || Options.encryption_key_format_versions) && Options.hls_version > 0 && Options.hls_version < 5) {
        Options.hls_version = 5;
        fprintf(stderr, "WARNING: forcing version to 5 in order to support KEYFORMAT and/or KEYFORMATVERSIONS\n");
    }
    if (Options.output_single_file && Options.hls_version > 0 && Options.hls_version < 4) {
        Options.hls_version = 4;
        fprintf(stderr, "WARNING: forcing version to 4 in order to support single file output\n");
    }
    if (Options.hls_version == 0) {
        // default version is 3 for cleartext or AES-128 encryption, and 5 for SAMPLE-AES
        if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES) {
            Options.hls_version = 5;
        } else if (Options.output_single_file) {
            Options.hls_version = 4;
        } else {
            Options.hls_version = 3;
        }
    }
    
    // compute some derived values
    if (Options.segment_filename_template == NULL) {
        if (Options.output_single_file) {
            Options.segment_filename_template = "stream.ts";
        } else {
            Options.segment_filename_template = "segment-%d.ts";
        }
    }
    if (Options.segment_url_template == NULL) {
        if (Options.output_single_file) {
            Options.segment_url_template = "stream.ts";
        } else {
            Options.segment_url_template = "segment-%d.ts";
        }
    }
    
    if (Options.encryption_iv_mode == ENCRYPTION_IV_MODE_FPS) {
        if (AP4_StringLength(Options.encryption_key_hex) != 64) {
            fprintf(stderr, "ERROR: 'fps' IV mode requires a 32 byte key value (64 characters in hex)\n");
            return 1;
        }
        result = AP4_ParseHex(Options.encryption_key_hex+32, Options.encryption_iv, 16);
        if (AP4_FAILED(result)) {
            fprintf(stderr, "ERROR: invalid hex IV\n");
            return 1;
        }
    } else if (Options.encryption_iv_mode == ENCRYPTION_IV_MODE_RANDOM) {
        AP4_Result result = AP4_System_GenerateRandomBytes(Options.encryption_iv, sizeof(Options.encryption_iv));
        if (AP4_FAILED(result)) {
            fprintf(stderr, "ERROR: failed to get random IV (%d)\n", result);
            return 1;
        }
    }
    
	// create the input stream
    AP4_ByteStream* input = NULL;
    result = AP4_FileByteStream::Create(Options.input, AP4_FileByteStream::STREAM_MODE_READ, input);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot open input (%d)\n", result);
        return 1;
    }
    
	// open the file
    AP4_File* input_file = new AP4_File(*input, AP4_DefaultAtomFactory::Instance, true);   

    // get the movie
    AP4_SampleDescription* sample_description;
    AP4_Movie* movie = input_file->GetMovie();
    if (movie == NULL) {
        fprintf(stderr, "ERROR: no movie in file\n");
        return 1;
    }

    // get the audio and video tracks
    AP4_Track* audio_track = movie->GetTrack(AP4_Track::TYPE_AUDIO);
    AP4_Track* video_track = movie->GetTrack(AP4_Track::TYPE_VIDEO);
    if (audio_track == NULL && video_track == NULL) {
        fprintf(stderr, "ERROR: no suitable tracks found\n");
        delete input_file;
        input->Release();
        return 1;
    }

    // create the appropriate readers
    AP4_LinearReader* linear_reader = NULL;
    SampleReader*     audio_reader  = NULL;
    SampleReader*     video_reader  = NULL;
    if (movie->HasFragments()) {
        // create a linear reader to get the samples
        linear_reader = new AP4_LinearReader(*movie, input);
    
        if (audio_track) {
            linear_reader->EnableTrack(audio_track->GetId());
            audio_reader = new FragmentedSampleReader(*linear_reader, audio_track->GetId());
        }
        if (video_track) {
            linear_reader->EnableTrack(video_track->GetId());
            video_reader = new FragmentedSampleReader(*linear_reader, video_track->GetId());
        }
    } else {
        if (audio_track) {
            audio_reader = new TrackSampleReader(*audio_track);
        }
        if (video_track) {
            video_reader = new TrackSampleReader(*video_track);
        }
    }
    
    // create an MPEG2 TS Writer
    AP4_Mpeg2TsWriter writer(Options.pmt_pid);
    AP4_Mpeg2TsWriter::SampleStream* audio_stream = NULL;
    AP4_Mpeg2TsWriter::SampleStream* video_stream = NULL;
    AP4_UI08 nalu_length_size = 0;

    // add the audio stream
    if (audio_track) {
        sample_description = audio_track->GetSampleDescription(0);
        if (sample_description == NULL) {
            fprintf(stderr, "ERROR: unable to parse audio sample description\n");
            goto end;
        }

        unsigned int stream_type = 0;
        unsigned int stream_id   = 0;
        if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_MP4A) {
            if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES) {
                stream_type = AP4_MPEG2_STREAM_TYPE_SAMPLE_AES_ISO_IEC_13818_7;
            } else {
                stream_type = AP4_MPEG2_STREAM_TYPE_ISO_IEC_13818_7;
            }
            stream_id   = AP4_MPEG2_TS_DEFAULT_STREAM_ID_AUDIO;
        } else if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AC_3 ||
                   sample_description->GetFormat() == AP4_SAMPLE_FORMAT_EC_3) {
            if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES) {
                stream_type = AP4_MPEG2_STREAM_TYPE_SAMPLE_AES_ATSC_AC3;
            } else {
                stream_type = AP4_MPEG2_STREAM_TYPE_ATSC_AC3;
            }
            stream_id   = AP4_MPEG2_TS_STREAM_ID_PRIVATE_STREAM_1;
        } else {
            fprintf(stderr, "ERROR: audio codec not supported\n");
            return 1;
        }

        // construct an extra descriptor if needed
        AP4_DataBuffer descriptor;
        if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES) {
            // descriptor
            descriptor.SetDataSize(6);
            AP4_UI08* payload = descriptor.UseData();
            payload[0] = AP4_MPEG2_PRIVATE_DATA_INDICATOR_DESCRIPTOR_TAG;
            payload[1] = 4;
            if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_MP4A) {
                payload[2] = 'a';
                payload[3] = 'a';
                payload[4] = 'c';
                payload[5] = 'd';
            } else if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AC_3 ||
                       sample_description->GetFormat() == AP4_SAMPLE_FORMAT_EC_3) {
                payload[2] = 'a';
                payload[3] = 'c';
                payload[4] = '3';
                payload[5] = 'd';
            }

            // audio info
            if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_MP4A) {
                AP4_MpegAudioSampleDescription* mpeg_audio_desc = AP4_DYNAMIC_CAST(AP4_MpegAudioSampleDescription, sample_description);
                if (mpeg_audio_desc == NULL ||
                    !(mpeg_audio_desc->GetObjectTypeId() == AP4_OTI_MPEG4_AUDIO          ||
                      mpeg_audio_desc->GetObjectTypeId() == AP4_OTI_MPEG2_AAC_AUDIO_LC   ||
                      mpeg_audio_desc->GetObjectTypeId() == AP4_OTI_MPEG2_AAC_AUDIO_MAIN)) {
                    fprintf(stderr, "ERROR: only AAC audio is supported\n");
                    return 1;
                }
                const AP4_DataBuffer& dsi = mpeg_audio_desc->GetDecoderInfo();
                AP4_Mp4AudioDecoderConfig dec_config;
                AP4_Result result = dec_config.Parse(dsi.GetData(), dsi.GetDataSize());
                if (AP4_FAILED(result)) {
                    fprintf(stderr, "ERROR: failed to parse decoder specific info (%d)\n", result);
                    return 1;
                }
                descriptor.SetDataSize(descriptor.GetDataSize()+14+dsi.GetDataSize());
                payload = descriptor.UseData()+6;
                payload[0] = AP4_MPEG2_REGISTRATION_DESCRIPTOR_TAG;
                payload[1] = 12+dsi.GetDataSize();
                payload[2] = 'a';
                payload[3] = 'p';
                payload[4] = 'a';
                payload[5] = 'd';
                payload += 6;
                if (dec_config.m_Extension.m_SbrPresent || dec_config.m_Extension.m_PsPresent) {
                    if (dec_config.m_Extension.m_PsPresent) {
                        payload[0] = 'z';
                        payload[1] = 'a';
                        payload[2] = 'c';
                        payload[3] = 'p';
                    } else {
                        payload[0] = 'z';
                        payload[1] = 'a';
                        payload[2] = 'c';
                        payload[3] = 'h';
                    }
                } else {
                    payload[0] = 'z';
                    payload[1] = 'a';
                    payload[2] = 'a';
                    payload[3] = 'c';
                }
                payload[4] = 0; // priming
                payload[5] = 0; // priming
                payload[6] = 1; // version
                payload[7] = dsi.GetDataSize(); // setup_data_length
                AP4_CopyMemory(&payload[8], dsi.GetData(), dsi.GetDataSize());
            } else if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AC_3 ||
                       sample_description->GetFormat() == AP4_SAMPLE_FORMAT_EC_3) {
                fprintf(stderr, "ERROR: AC3 support not fully implemented yet\n");
                return 1;
            }
        }

        // setup the audio stream
        result = writer.SetAudioStream(audio_track->GetMediaTimeScale(),
                                       stream_type,
                                       stream_id,
                                       audio_stream,
                                       Options.audio_pid,
                                       descriptor.GetDataSize()?descriptor.GetData():NULL,
                                       descriptor.GetDataSize());
        if (AP4_FAILED(result)) {
            fprintf(stderr, "could not create audio stream (%d)\n", result);
            goto end;
        }
    }
    
    // add the video stream
    if (video_track) {
        sample_description = video_track->GetSampleDescription(0);
        if (sample_description == NULL) {
            fprintf(stderr, "ERROR: unable to parse video sample description\n");
            goto end;
        }
        
        // decide on the stream type
        unsigned int stream_type = 0;
        unsigned int stream_id   = AP4_MPEG2_TS_DEFAULT_STREAM_ID_VIDEO;
        if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AVC1 ||
            sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AVC2 ||
            sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AVC3 ||
            sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AVC4) {
            if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES) {
                stream_type = AP4_MPEG2_STREAM_TYPE_SAMPLE_AES_AVC;
                AP4_AvcSampleDescription* avc_desc = AP4_DYNAMIC_CAST(AP4_AvcSampleDescription, sample_description);
                if (avc_desc == NULL) {
                    fprintf(stderr, "ERROR: not a proper AVC track\n");
                    return 1;
                }
                nalu_length_size = avc_desc->GetNaluLengthSize();
            } else {
                stream_type = AP4_MPEG2_STREAM_TYPE_AVC;
            }
        } else if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_HEV1 ||
                   sample_description->GetFormat() == AP4_SAMPLE_FORMAT_HVC1) {
            stream_type = AP4_MPEG2_STREAM_TYPE_HEVC;
        } else {
            fprintf(stderr, "ERROR: video codec not supported\n");
            return 1;
        }
        if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES) {
            if (stream_type != AP4_MPEG2_STREAM_TYPE_SAMPLE_AES_AVC) {
                fprintf(stderr, "ERROR: AES-SAMPLE encryption can only be used with H.264 video\n");
                return 1;
            }
        }
        
        // construct an extra descriptor if needed
        AP4_DataBuffer descriptor;
        if (Options.encryption_mode == ENCRYPTION_MODE_SAMPLE_AES) {
            descriptor.SetDataSize(6);
            AP4_UI08* payload = descriptor.UseData();
            payload[0] = AP4_MPEG2_PRIVATE_DATA_INDICATOR_DESCRIPTOR_TAG;
            payload[1] = 4;
            payload[2] = 'z';
            payload[3] = 'a';
            payload[4] = 'v';
            payload[5] = 'c';
        }

        // setup the video stream
        result = writer.SetVideoStream(video_track->GetMediaTimeScale(),
                                       stream_type,
                                       stream_id,
                                       video_stream,
                                       Options.video_pid,
                                       descriptor.GetDataSize()?descriptor.GetData():NULL,
                                       descriptor.GetDataSize());
        if (AP4_FAILED(result)) {
            fprintf(stderr, "could not create video stream (%d)\n", result);
            goto end;
        }
    }
    
    result = WriteSamples(writer,
                          audio_track, audio_reader, audio_stream,
                          video_track, video_reader, video_stream,
                          Options.segment_duration_threshold,
                          nalu_length_size);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: failed to write samples (%d)\n", result);
    }

end:
    delete input_file;
    input->Release();
    delete linear_reader;
    delete audio_reader;
    delete video_reader;
    
    return result == AP4_SUCCESS?0:1;
}

