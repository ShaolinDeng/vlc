/*****************************************************************************
 * asfpacket.c :
 *****************************************************************************
 * Copyright © 2001-2004, 2011, 2014 VLC authors and VideoLAN
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "asfpacket.h"
#include <limits.h>

#ifndef NDEBUG
# define ASF_DEBUG 1
#endif

typedef struct asf_packet_t
{
    uint32_t property;
    uint32_t length;
    uint32_t padding_length;
    uint32_t send_time;
    bool multiple;
    int length_type;

    /* buffer handling for this ASF packet */
    uint32_t i_skip;
    const uint8_t *p_peek;
    uint32_t left;
} asf_packet_t;

static inline int GetValue2b(uint32_t *var, const uint8_t *p, unsigned int *skip, int left, int bits)
{
    switch(bits&0x03)
    {
    case 1:
        if (left < 1)
            return -1;
        *var = p[*skip]; *skip += 1;
        return 0;
    case 2:
        if (left < 2)
            return -1;
        *var = GetWLE(&p[*skip]); *skip += 2;
        return 0;
    case 3:
        if (left < 4)
            return -1;
        *var = GetDWLE(&p[*skip]); *skip += 4;
        return 0;
    case 0:
    default:
        return 0;
    }
}

static uint32_t SkipBytes( stream_t *s, uint32_t i_bytes )
{
    int i_read;
    int i_to_read = __MIN(i_bytes, INT_MAX);
    uint32_t i_bytes_read = 0;

    while( i_bytes )
    {
        i_read = stream_Read( s, NULL, i_to_read );
        i_bytes -= i_read;
        i_bytes_read += i_read;
        if ( i_read < i_to_read || i_bytes == 0 )
        {
            /* end of stream */
            return i_bytes_read;
        }
        i_to_read = __MIN(i_bytes, INT_MAX);
    }

    return i_bytes_read;
}

static int DemuxSubPayload( asf_packet_sys_t *p_packetsys,
                            uint8_t i_stream_number, block_t **pp_frame,
                            uint32_t i_sub_payload_data_length, mtime_t i_pts, mtime_t i_dts,
                            uint32_t i_media_object_offset, bool b_keyframe )
{
    /* FIXME I don't use i_media_object_number, sould I ? */
    if( *pp_frame && i_media_object_offset == 0 )
    {
        p_packetsys->pf_send( p_packetsys, i_stream_number, pp_frame );
    }

    block_t *p_frag = stream_Block( p_packetsys->p_demux->s, i_sub_payload_data_length );
    if( p_frag == NULL ) {
        msg_Warn( p_packetsys->p_demux, "cannot read data" );
        return -1;
    }

    p_frag->i_pts = VLC_TS_0 + i_pts;
    p_frag->i_dts = VLC_TS_0 + i_dts;
    if ( b_keyframe )
        p_frag->i_flags |= BLOCK_FLAG_TYPE_I;

    block_ChainAppend( pp_frame, p_frag );

    return 0;
}

static void ParsePayloadExtensions( asf_packet_sys_t *p_packetsys,
                                    const asf_track_info_t *p_tkinfo,
                                    const asf_packet_t *pkt,
                                    uint32_t i_length, bool *b_keyframe )
{
    demux_t *p_demux = p_packetsys->p_demux;

    if ( !p_tkinfo || !p_tkinfo->p_esp || !p_tkinfo->p_esp->p_ext ) return;
    const uint8_t *p_data = pkt->p_peek + pkt->i_skip + 8;
    i_length -= 8;
    uint16_t i_payload_extensions_size;
    asf_payload_extension_system_t *p_ext = NULL;

    /* Extensions always come in the declared order */
    for ( int i=0; i< p_tkinfo->p_esp->i_payload_extension_system_count; i++ )
    {
        p_ext = &p_tkinfo->p_esp->p_ext[i];
        if ( p_ext->i_data_size == 0xFFFF ) /* Variable length extension data */
        {
            if ( i_length < 2 ) return;
            i_payload_extensions_size = GetWLE( p_data );
            p_data += 2;
            i_length -= 2;
            i_payload_extensions_size = 0;
        }
        else
        {
            i_payload_extensions_size = p_ext->i_data_size;
        }

        if ( i_length < i_payload_extensions_size ) return;

        if ( guidcmp( &p_ext->i_extension_id, &mfasf_sampleextension_outputcleanpoint_guid ) )
        {
            if ( i_payload_extensions_size != sizeof(uint8_t) ) goto sizeerror;
            *b_keyframe |= *p_data;
        }
        else if ( guidcmp( &p_ext->i_extension_id, &asf_dvr_sampleextension_videoframe_guid ) )
        {
            if ( i_payload_extensions_size != sizeof(uint32_t) ) goto sizeerror;

            uint32_t i_val = GetDWLE( p_data );
            /* Valid keyframe must be a split frame start fragment */
            *b_keyframe = i_val & ASF_EXTENSION_VIDEOFRAME_NEWFRAME;
            if ( *b_keyframe )
            {
                /* And flagged as IFRAME */
                *b_keyframe |= ( ( i_val & ASF_EXTENSION_VIDEOFRAME_TYPE_MASK )
                                 == ASF_EXTENSION_VIDEOFRAME_IFRAME );
            }
        }
        else if ( guidcmp( &p_ext->i_extension_id, &mfasf_sampleextension_pixelaspectratio_guid ) &&
                  p_packetsys->pf_setaspectratio )
        {
            if ( i_payload_extensions_size != sizeof(uint16_t) ) goto sizeerror;

            p_packetsys->pf_setaspectratio( p_packetsys, p_tkinfo->p_sp->i_stream_number,
                                            p_data[0], p_data[1] );
        }
        i_length -= i_payload_extensions_size;
        p_data += i_payload_extensions_size;
    }

    return;

sizeerror:
    msg_Warn( p_demux, "Unknown extension " GUID_FMT " data size of %u",
              GUID_PRINT( p_ext->i_extension_id ), i_payload_extensions_size );
}

static int DemuxPayload(asf_packet_sys_t *p_packetsys, asf_packet_t *pkt, int i_payload)
{
#ifndef ASF_DEBUG
    VLC_UNUSED( i_payload );
#endif
    demux_t *p_demux = p_packetsys->p_demux;

    if( ! pkt->left || pkt->i_skip >= pkt->left )
        return -1;

    bool b_packet_keyframe = pkt->p_peek[pkt->i_skip] >> 7;
    uint8_t i_stream_number = pkt->p_peek[pkt->i_skip++] & 0x7f;
    if ( i_stream_number > ASF_MAX_STREAMNUMBER )
        goto skip;

    uint32_t i_media_object_number = 0;
    if (GetValue2b(&i_media_object_number, pkt->p_peek, &pkt->i_skip, pkt->left - pkt->i_skip, pkt->property >> 4) < 0)
        return -1;
    uint32_t i_media_object_offset = 0;
    if (GetValue2b(&i_media_object_offset, pkt->p_peek, &pkt->i_skip, pkt->left - pkt->i_skip, pkt->property >> 2) < 0)
        return -1;
    uint32_t i_replicated_data_length = 0;
    if (GetValue2b(&i_replicated_data_length, pkt->p_peek, &pkt->i_skip, pkt->left - pkt->i_skip, pkt->property) < 0)
        return -1;

    mtime_t i_base_pts;
    uint8_t i_pts_delta = 0;
    uint32_t i_payload_data_length = 0;
    uint32_t i_temp_payload_length = 0;
    *p_packetsys->pi_preroll = __MIN( *p_packetsys->pi_preroll, INT64_MAX );

    /* First packet, in case we do not have index to guess preroll start time */
    if ( *p_packetsys->pi_preroll_start == ASFPACKET_PREROLL_FROM_CURRENT )
        *p_packetsys->pi_preroll_start = pkt->send_time * 1000;

    asf_track_info_t *p_tkinfo = p_packetsys->pf_gettrackinfo( p_packetsys, i_stream_number );
    if ( !p_tkinfo )
        goto skip;

    /* Non compressed */
    if( i_replicated_data_length > 7 ) // should be at least 8 bytes
    {
        /* Followed by 2 optional DWORDS, offset in media and presentation time */
        i_base_pts = (mtime_t)GetDWLE( pkt->p_peek + pkt->i_skip + 4 );

        /* Parsing extensions, See 7.3.1 */
        ParsePayloadExtensions( p_packetsys, p_tkinfo, pkt,
                                i_replicated_data_length, &b_packet_keyframe );
        i_base_pts -= *p_packetsys->pi_preroll;
        pkt->i_skip += i_replicated_data_length;

        if( ! pkt->left || pkt->i_skip >= pkt->left )
            return -1;
    }
    else if ( i_replicated_data_length == 0 )
    {
        /* optional DWORDS missing */
        i_base_pts = (mtime_t)pkt->send_time;
        i_base_pts -= *p_packetsys->pi_preroll;
    }
    /* Compressed payload */
    else if( i_replicated_data_length == 1 )
    {
        /* i_media_object_offset is presentation time */
        /* Next byte is Presentation Time Delta */
        i_pts_delta = pkt->p_peek[pkt->i_skip];
        i_base_pts = (mtime_t)i_media_object_offset;
        i_base_pts -= *p_packetsys->pi_preroll;
        pkt->i_skip++;
        i_media_object_offset = 0;
    }
    else
    {
        /* >1 && <8 Invalid replicated length ! */
        msg_Warn( p_demux, "Invalid replicated data length detected." );
        i_payload_data_length = pkt->length - pkt->padding_length - pkt->i_skip;
        goto skip;
    }

    bool b_preroll_done = ( pkt->send_time > (*p_packetsys->pi_preroll_start/1000 + *p_packetsys->pi_preroll) );

    if (i_base_pts < 0) i_base_pts = 0; // FIXME?
    i_base_pts *= 1000;

    if( pkt->multiple ) {
        if (GetValue2b(&i_temp_payload_length, pkt->p_peek, &pkt->i_skip, pkt->left - pkt->i_skip, pkt->length_type) < 0)
            return -1;
    } else
        i_temp_payload_length = pkt->length - pkt->padding_length - pkt->i_skip;

    i_payload_data_length = i_temp_payload_length;

#ifdef ASF_DEBUG
     msg_Dbg( p_demux,
              "payload(%d) stream_number:%"PRIu8" media_object_number:%d media_object_offset:%"PRIu32" replicated_data_length:%"PRIu32" payload_data_length %"PRIu32,
              i_payload + 1, i_stream_number, i_media_object_number,
              i_media_object_offset, i_replicated_data_length, i_payload_data_length );
     msg_Dbg( p_demux,
              "   pts=%"PRId64" st=%"PRIu32, i_base_pts, pkt->send_time );
#endif

     if( ! i_payload_data_length || i_payload_data_length > pkt->left )
     {
         msg_Dbg( p_demux, "  payload length problem %d %"PRIu32" %"PRIu32, pkt->multiple, i_payload_data_length, pkt->left );
         return -1;
     }

    if ( p_packetsys->pf_doskip &&
         p_packetsys->pf_doskip( p_packetsys, i_stream_number, b_packet_keyframe ) )
        goto skip;

    bool b_hugedelay = ( *p_packetsys->pi_preroll * 1000 > CLOCK_FREQ * 3 );

    if ( b_preroll_done || b_hugedelay )
    {
        mtime_t i_track_time;
        if ( !b_hugedelay )
        {
            i_track_time = INT64_C(1000) * pkt->send_time;
            i_track_time -= *p_packetsys->pi_preroll * 1000;
            if ( p_tkinfo->p_sp )
                i_track_time -= p_tkinfo->p_sp->i_time_offset * 10;
        }
        else
            i_track_time = i_base_pts;

        if ( p_packetsys->pf_updatetime )
            p_packetsys->pf_updatetime( p_packetsys, i_stream_number, i_track_time );
    }

    uint32_t i_subpayload_count = 0;
    while (i_payload_data_length)
    {
        uint32_t i_sub_payload_data_length = i_payload_data_length;
        if( i_replicated_data_length == 1 )
        {
            i_sub_payload_data_length = pkt->p_peek[pkt->i_skip++];
            i_payload_data_length--;
        }

        SkipBytes( p_demux->s, pkt->i_skip );

        mtime_t i_payload_pts = i_base_pts + (mtime_t)i_pts_delta * i_subpayload_count * 1000;
        if ( p_tkinfo->p_sp )
            i_payload_pts -= p_tkinfo->p_sp->i_time_offset * 10;

        mtime_t i_payload_dts = INT64_C(1000) * pkt->send_time;
        i_payload_dts -= *p_packetsys->pi_preroll * 1000;
        if ( p_tkinfo->p_sp )
            i_payload_dts -= p_tkinfo->p_sp->i_time_offset * 10;

        if ( b_hugedelay )
            i_payload_dts = i_base_pts;

        if ( i_sub_payload_data_length &&
             DemuxSubPayload( p_packetsys, i_stream_number, &p_tkinfo->p_frame,
                              i_sub_payload_data_length, i_payload_pts, i_payload_dts,
                              i_media_object_offset, b_packet_keyframe ) < 0)
            return -1;

        if ( pkt->left > pkt->i_skip + i_sub_payload_data_length )
            pkt->left -= pkt->i_skip + i_sub_payload_data_length;
        else
            pkt->left = 0;
        pkt->i_skip = 0;
        if( pkt->left > 0 )
        {
            int i_return = stream_Peek( p_demux->s, &pkt->p_peek, __MIN(pkt->left, INT_MAX) );
            if ( i_return <= 0 || (unsigned int) i_return < __MIN(pkt->left, INT_MAX) )
            {
            msg_Warn( p_demux, "cannot peek, EOF ?" );
            return -1;
            }
        }

        if ( i_sub_payload_data_length <= i_payload_data_length )
            i_payload_data_length -= i_sub_payload_data_length;
        else
            i_payload_data_length = 0;

        i_subpayload_count++;
    }

    return 0;

skip:
    pkt->i_skip += i_payload_data_length;
    return 0;
}

int DemuxASFPacket( asf_packet_sys_t *p_packetsys,
                 uint32_t i_data_packet_min, uint32_t i_data_packet_max )
{
    demux_t *p_demux = p_packetsys->p_demux;

    const uint8_t *p_peek;
    int i_return = stream_Peek( p_demux->s, &p_peek,i_data_packet_min );
    if( i_return <= 0 || ((unsigned int) i_return) < i_data_packet_min )
    {
        msg_Warn( p_demux, "cannot peek while getting new packet, EOF ?" );
        return 0;
    }
    unsigned int i_skip = 0;

    /* *** parse error correction if present *** */
    if( p_peek[0]&0x80 )
    {
        unsigned int i_error_correction_data_length = p_peek[0] & 0x0f;
        unsigned int i_opaque_data_present = ( p_peek[0] >> 4 )& 0x01;
        unsigned int i_error_correction_length_type = ( p_peek[0] >> 5 ) & 0x03;
        i_skip += 1; // skip error correction flags

        if( i_error_correction_length_type != 0x00 ||
            i_opaque_data_present != 0 ||
            i_error_correction_data_length != 0x02 )
        {
            goto loop_error_recovery;
        }

        i_skip += i_error_correction_data_length;
    }
    else
        msg_Warn( p_demux, "no error correction" );

    /* sanity check */
    if( i_skip + 2 >= i_data_packet_min )
        goto loop_error_recovery;

    asf_packet_t pkt;
    int i_packet_flags = p_peek[i_skip]; i_skip++;
    pkt.property = p_peek[i_skip]; i_skip++;
    pkt.multiple = !!(i_packet_flags&0x01);

    pkt.length = i_data_packet_min;
    pkt.padding_length = 0;

    if (GetValue2b(&pkt.length, p_peek, &i_skip, i_data_packet_min - i_skip, i_packet_flags >> 5) < 0)
        goto loop_error_recovery;
    uint32_t i_packet_sequence;
    if (GetValue2b(&i_packet_sequence, p_peek, &i_skip, i_data_packet_min - i_skip, i_packet_flags >> 1) < 0)
        goto loop_error_recovery;
    if (GetValue2b(&pkt.padding_length, p_peek, &i_skip, i_data_packet_min - i_skip, i_packet_flags >> 3) < 0)
        goto loop_error_recovery;

    if( pkt.padding_length > pkt.length )
    {
        msg_Warn( p_demux, "Too large padding: %"PRIu32, pkt.padding_length );
        goto loop_error_recovery;
    }

    if( pkt.length < i_data_packet_min )
    {
        /* if packet length too short, there is extra padding */
        pkt.padding_length += i_data_packet_min - pkt.length;
        pkt.length = i_data_packet_min;
    }

    pkt.send_time = GetDWLE( p_peek + i_skip ); i_skip += 4;
    /* uint16_t i_packet_duration = GetWLE( p_peek + i_skip ); */ i_skip += 2;

    i_return = stream_Peek( p_demux->s, &p_peek, pkt.length );
    if( i_return <= 0 || pkt.length == 0 || (unsigned int)i_return < pkt.length )
    {
        msg_Warn( p_demux, "cannot peek, EOF ?" );
        return 0;
    }

    int i_payload_count = 1;
    pkt.length_type = 0x02; //unused
    if( pkt.multiple )
    {
        i_payload_count = p_peek[i_skip] & 0x3f;
        pkt.length_type = ( p_peek[i_skip] >> 6 )&0x03;
        i_skip++;
    }

#ifdef ASF_DEBUG
    msg_Dbg(p_demux, "%d payloads", i_payload_count);
#endif

    pkt.i_skip = i_skip;
    pkt.p_peek = p_peek;
    pkt.left = pkt.length;

    for( int i_payload = 0; i_payload < i_payload_count ; i_payload++ )
        if (DemuxPayload(p_packetsys, &pkt, i_payload) < 0)
        {
            msg_Warn( p_demux, "payload err %d / %d", i_payload + 1, i_payload_count );
            return 0;
        }

    if( pkt.left > 0 )
    {
#ifdef ASF_DEBUG
        if( pkt.left > pkt.padding_length )
            msg_Warn( p_demux, "Didn't read %"PRIu32" bytes in the packet",
                            pkt.left - pkt.padding_length );
        else if( pkt.left < pkt.padding_length )
            msg_Warn( p_demux, "Read %"PRIu32" too much bytes in the packet",
                            pkt.padding_length - pkt.left );
#endif
        int i_return = stream_Read( p_demux->s, NULL, pkt.left );
        if( i_return < 0 || (unsigned int) i_return < pkt.left )
        {
            msg_Err( p_demux, "cannot skip data, EOF ?" );
            return 0;
        }
    }

    return 1;

loop_error_recovery:
    msg_Warn( p_demux, "unsupported packet header" );
    if( i_data_packet_min != i_data_packet_max )
    {
        msg_Err( p_demux, "unsupported packet header, fatal error" );
        return -1;
    }
    i_return = stream_Read( p_demux->s, NULL, i_data_packet_min );
    if( i_return <= 0 || (unsigned int) i_return != i_data_packet_min )
    {
        msg_Warn( p_demux, "cannot skip data, EOF ?" );
        return 0;
    }

    return 1;
}
