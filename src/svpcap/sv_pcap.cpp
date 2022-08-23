/*****************************************************************************
 *
 * sv_pcap.cpp
 *   Code for collecting network capture on demand. Used by SV
 *   for troubleshooing of camera connection on demand.
 *
 *****************************************************************************
 *
 * Copyright 2013-2022 Sighthound, Inc.
 *
 * Licensed under the GNU GPLv3 license found at
 * https://www.gnu.org/licenses/gpl-3.0.txt
 *
 * Alternative licensing available from Sighthound, Inc.
 * by emailing opensource@sighthound.com
 *
 * This file is part of the Sighthound Video project which can be found at
 * https://github.url/thing
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; using version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 *****************************************************************************/

#include "sv_pcap.h"


//-----------------------------------------------------------------------------
extern "C" {
#include <pcap/pcap.h>
#ifdef _WIN32
    #include <Packet32.h>
#endif
}

#include <algorithm>

//-----------------------------------------------------------------------------
typedef struct sv_pcap_dev {
    sv_pcap_log_cb_t logCb;
    void*           logCtx;
    pcap_t*         handle;
    pcap_dumper_t*  dumper;
    char            errbuf[PCAP_ERRBUF_SIZE];
    char*           device;
    char*           path;
    sv_thread*      thread;
    INT64_T         captureStart;
    size_t          captureSize;
    size_t          packetsWritten;
    size_t          maxMsec;
    size_t          maxBytes;
    int             running;
    struct bpf_program  fp;      /* The compiled filter expression */
} sv_pcap_dev;

typedef struct sv_pcap {
    enum { MaxDevices = 16 };
    sv_pcap_dev*    caps[MaxDevices];
    int             activeCaps;
    char*           ip;
    char            errbuf[PCAP_ERRBUF_SIZE];
} sv_pcap;

//-----------------------------------------------------------------------------
static void     _sv_stop_capture(sv_pcap_dev** ppcap);


//-----------------------------------------------------------------------------
// Very crude attempt to creat a pcap filter from an RTSP/HTTP URI
static char* _sv_ip_from_uri(const char* _url,
                            sv_pcap_log_cb_t logCb,
                            void* logCtx)
{
    // TODO: very crude ... may need improvement
    char *protoEnd, *userEnd, *qm, *slash, *host=NULL, *port=NULL;
    char* url=(char*)_url;
    int len, res;
    struct in_addr addr;
    struct addrinfo hints,*ai,*ptr;
    struct sockaddr_in* sockaddr_ipv4;

    if (url == NULL) {
        return NULL;
    }

    logCb(logCtx, svpllTrace, _STR("Attempting to enable packet trace for <" << url << ">"));

    protoEnd = strstr(url, "://");
    if (protoEnd) {
        url = protoEnd + 3;
        logCb(logCtx, svpllTrace, _STR("Skipped protocol ... remaining=" << url));
    }
    userEnd = strchr(url, '@');
    if ( userEnd != NULL ) {
        url = userEnd+1;
        logCb(logCtx, svpllTrace, _STR("Skipped user/pwd ... remaining=" << url));
    }
    qm = strchr(url, '?');
    slash = strchr(url, '/');
    len = strlen(url);
    if ( slash != NULL && qm != NULL ) {
        len = len - std::max(strlen(qm), strlen(slash));
    } else if ( qm != NULL ) {
        len = len - strlen(qm);
    } else if ( slash != NULL ) {
        len = len - strlen(slash);
    }

    host = (char*)malloc(len+1);
    strncpy(host, url, len);
    host[len] = '\0';
    logCb(logCtx, svpllTrace, _STR("Removed path ... remaining=" << host));

    port = strchr(host, ':');
    if (port != NULL) {
        logCb(logCtx, svpllTrace, _STR("Removed port ... remaining=" << host));
        *port = '\0';
    }


    logCb(logCtx, svpllTrace, _STR("Host=" << host));
    addr.s_addr = inet_addr(host);
    if ( addr.s_addr == INADDR_NONE ) {
        memset(&hints,0,sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;


        res = getaddrinfo(host, "http", &hints, &ai);
        sv_freep(&host);

        if ( res == 0 ) {
            for(ptr=ai; ptr != NULL && host == NULL;ptr=ptr->ai_next) {
                switch (ptr->ai_family) {
                case AF_INET:
                    sockaddr_ipv4 = (struct sockaddr_in *) ptr->ai_addr;
                    host =  strdup(inet_ntoa(sockaddr_ipv4->sin_addr));
                    break;
                case AF_INET6:
                default:
                    break;
                }
            }
            freeaddrinfo(ai);
        }
    }


    return host;
}


//-----------------------------------------------------------------------------
// never got the callback working reliably
#if 0
static void _sv_pcap_callback(u_char *arg,
                        const struct pcap_pkthdr* pcap_hdr,
                        const u_char* packet)
{
    //PCAP_DEBUG("_sv_pcap_callback -- in");
    sv_pcap* pcap = (sv_pcap*)arg;
    size_t size = pcap_hdr->len;

    if (!pcap->running ||
        (sv_time_get_elapsed_time(pcap->captureStart) > pcap->maxMsec && pcap->maxMsec) ||
        (size + pcap->captureSize > pcap->maxBytes && pcap->maxBytes) ) {
        //PCAP_DEBUG("pcap_breakloop");
        pcap_breakloop(pcap->handle);
        return;
    }

    // save the packet
    //PCAP_DEBUG("pcap_dump");
    pcap_dump((u_char *)pcap->dumper, pcap_hdr, packet);
    pcap->packetsWritten++;

    pcap->captureSize += size;
    //PCAP_DEBUG("_sv_pcap_callback -- out");
}
#endif

//-----------------------------------------------------------------------------
static void* _sv_pcap_thread(void* arg)
{
    sv_pcap_dev* pcap = (sv_pcap_dev*)arg;
    pcap->logCb(pcap->logCtx, svpllDebug, _FMT("_sv_pcap_thread - dev=" << pcap->device << " in - " << (void*)arg));
    while (pcap->running) {
        struct pcap_pkthdr *pkt_header;
        const u_char *pkt_data;
        int res = pcap_next_ex(pcap->handle, &pkt_header, &pkt_data);
        if ( res >= 0 ) {
            size_t size = pkt_header->len;

            if ( (sv_time_get_elapsed_time(pcap->captureStart) > pcap->maxMsec && pcap->maxMsec) ||
                 (size + pcap->captureSize > pcap->maxBytes && pcap->maxBytes) ) {
                break;
            }

            if ( res > 0 ) {
                // save the packet
                pcap->logCb(pcap->logCtx, svpllTrace, _FMT(pcap->device << "- pcap_dump: pkt_header->len=" << pkt_header->len <<
                                         " pkt_header->caplen=" << pkt_header->caplen));
                pcap_dump((u_char *)pcap->dumper, pkt_header, pkt_data);
                pcap->packetsWritten++;

                pcap->captureSize += size;
            }
        }
    }

    struct pcap_stat stat;
    if ( pcap_stats(pcap->handle, &stat) ) {

    }
    pcap->logCb(pcap->logCtx, svpllInfo, _FMT("_sv_pcap_thread - out: dev=" << pcap->device << " rx=" << stat.ps_recv << " drop=" << stat.ps_drop << " if_drop=" << stat.ps_ifdrop << " written=" << pcap->packetsWritten));
    return NULL;
}

//-----------------------------------------------------------------------------
static bool _sv_capture_can_use_device( pcap_if_t* device )
{
    bool canUseDevice = false;

    pcap_addr_t* addr = device->addresses;

    while (addr != NULL && !canUseDevice) {
        if (addr->addr->sa_family == AF_INET ||
            addr->addr->sa_family == AF_INET6 ) {
            canUseDevice = true;
        }
        addr = addr->next;
    }
    return canUseDevice;
}

//-----------------------------------------------------------------------------
char* _sv_capture_filter_name(char* name)
{
    char* ret = strdup(name);
    char* s = ret;
    while (*s) {
        if (!isalnum(*s)) {
            *s='_';
        }
        s++;
    }
    return ret;
}

//-----------------------------------------------------------------------------
static sv_pcap_dev* _sv_capture_traffic(const char* ipStr,
                                   pcap_if_t* device,
                                   const char* saveTo,
                                   size_t maxCaptureMsec,
                                   size_t maxCaptureSizeBytes,
                                   sv_pcap_log_cb_t logCb,
                                   void* logCtx)
{
    char                filter[256] = "\0";
    char*               filteredName = NULL;
    sv_pcap_dev* res = (sv_pcap_dev*)malloc(sizeof(sv_pcap));
    memset(res, 0, sizeof(sv_pcap_dev));
    res->logCb = logCb;
    res->logCtx = logCtx;

    const char* desc = device->description;
    if (!desc) {
        desc = "No description available";
    }

    logCb(logCtx, svpllInfo, _FMT("Attempting to start capture on device '" << device->name << "'" << "(" << desc << ")"));


    res->device = strdup(device->name);
    res->maxBytes = maxCaptureSizeBytes;
    res->maxBytes = maxCaptureMsec;
    res->packetsWritten = 0;
    res->captureSize = 0;


    res->handle = pcap_open_live(res->device, 65535, 0, 1000, res->errbuf);
    if (res->handle == NULL) {
        logCb( logCtx, svpllError, _STR("Failed to open live capture"));
        goto Error;
    }

    filteredName = _sv_capture_filter_name(device->name);
    res->path = strdup(_STR(saveTo<<"-"<<filteredName<<".pcap"));
    sv_freep(&filteredName);

    res->dumper = pcap_dump_open(res->handle, res->path);
    if (res->dumper == NULL) {
        logCb(logCtx, svpllError, _FMT("Failed to create dumper to " << res->path));
        goto Error;
    }

    if ( ipStr != NULL ) {
        bpf_u_int32         mask = (bpf_u_int32)inet_addr("255.255.255.0");      /* The netmask of our sniffing device */

        sprintf(filter, "host %s", ipStr);

        if (pcap_compile(res->handle, &res->fp, filter, 0, mask) == -1) {
            logCb( logCtx, svpllError, _STR("Failed to compile the filter"));
            goto Error;
        }
        if (pcap_setfilter(res->handle, &res->fp) == -1) {
            logCb( logCtx, svpllError, _STR("Failed to set the filter"));
            goto Error;
        }
    }

    res->captureStart = sv_time_get_current_epoch_time();
    res->running = 1;
    res->thread = sv_thread_create(_sv_pcap_thread, res );
    if ( !res->thread ) {
        logCb( logCtx, svpllError, _STR("Failed to start the filter"));
        goto Error;
    }

    logCb(logCtx, svpllInfo, _FMT("Started capture on device '" << device->name << "' with filter '" << (*filter?filter:"") << "'"));
    return res;

Error:
    _sv_stop_capture(&res);
    return NULL;
}

//-----------------------------------------------------------------------------
SVPCAP_API sv_pcap* sv_capture_traffic(const char* uri,
                                   const char* saveTo,
                                   size_t maxCaptureMsec,
                                   size_t maxCaptureSizeBytes,
                                   sv_pcap_log_cb_t logCb,
                                   void* logCtx)
{
    const char* ipStr = _sv_ip_from_uri(uri, logCb, logCtx);

    sv_pcap* res = (sv_pcap*)malloc(sizeof(sv_pcap));
    res->activeCaps = 0;


    pcap_if_t *alldevs=NULL, *dev_it=NULL;
    int ret = pcap_findalldevs(&alldevs, res->errbuf);
    if ( ret < 0 ) {
        logCb( logCtx, svpllError, _STR("Failed to retrive device list: err=" << ret));
        goto Error;
    }

    for(dev_it=alldevs; dev_it; dev_it=dev_it->next) {
        if ( !_sv_capture_can_use_device(dev_it) ) {
            continue;
        }
        sv_pcap_dev* newCap = _sv_capture_traffic(ipStr, dev_it, saveTo,
                                        maxCaptureMsec, maxCaptureSizeBytes,
                                        logCb, logCtx);
        if ( newCap != NULL ) {
            res->caps[res->activeCaps++] = newCap;
        }
    }

    if (alldevs!=NULL) {
        pcap_freealldevs(alldevs);
    }

Error:
    if ( res->activeCaps == 0 ) {
        sv_freep(&res);
    }
    sv_freep(&ipStr);
    return res;
}


//-----------------------------------------------------------------------------
static void     _sv_stop_capture(sv_pcap_dev** ppcap)
{
    if ( !ppcap || !*ppcap )
        return;
    sv_pcap_dev* pcap = *ppcap;
    pcap->logCb(pcap->logCtx, svpllDebug, _FMT("Attempting to stop capture on " << pcap->device));
    if ( pcap->thread ) {
        pcap->running = 0;
        pcap_breakloop(pcap->handle);
        sv_thread_destroy(&pcap->thread);
    }

    if (pcap->packetsWritten == 0 &&
        pcap->path != NULL &&
        pcap->dumper != NULL) {
        pcap->logCb(pcap->logCtx, svpllDebug, _FMT("Attempting to remove " << pcap->path));
        remove(pcap->path);
    }

    pcap->logCb(pcap->logCtx, svpllDebug, _FMT("Stopped capture on " << pcap->device));
    sv_freep(&pcap->device);
    sv_freep(&pcap->path);

    if ( pcap->dumper != NULL ) {
        pcap_dump_close(pcap->dumper);
    }

    if ( pcap->handle != NULL ) {
        pcap_close(pcap->handle);
    }
    sv_freep(ppcap);
}

//-----------------------------------------------------------------------------
SVPCAP_API void     sv_stop_capture(sv_pcap** ppcap)
{
    if ( !ppcap || !*ppcap )
        return;
    sv_pcap* pcap = *ppcap;
    for (int nI=0; nI<pcap->activeCaps; nI++) {
        _sv_stop_capture(&pcap->caps[nI]);
    }
    sv_freep(ppcap);
}

//-----------------------------------------------------------------------------
SVPCAP_API int      sv_capture_possible()
{
#ifdef _WIN32
    PCHAR verDrv = PacketGetDriverVersion();
    PCHAR ver = PacketGetVersion();
    //PCAP_DEBUG(_FMT("Ver=" << (ver?ver:"NULL") << " DrvVer=" << (verDrv?verDrv:"NULL")));
    // https://www.winpcap.org/pipermail/winpcap-users/2009-March/003062.html
    // "  due to a bug in packet.dll, at the moment PacketGetDriverVersion will
    // report an empty string when called from a 32bit process on a 64bit operating
    // system. "
    return ( verDrv != NULL && ver != NULL && /**verDrv &&*/ *ver ) ? 1 : -2;
#else
    return (getuid()==0 || geteuid()==0)? 1 : -1;
#endif
}

