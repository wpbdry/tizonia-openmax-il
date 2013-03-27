/**
 * Copyright (C) 2011-2013 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   icerprc.c
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief Tizonia OpenMAX IL - Icecast-like Http Sink processor class
 * implementation
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>

#include "OMX_Core.h"
#include "OMX_TizoniaExt.h"

#include "tizkernel.h"
#include "tizscheduler.h"

#include "icerprc_decls.h"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.http_renderer.prc"
#endif

static OMX_ERRORTYPE
stream_to_clients (struct icerprc *ap_obj, OMX_HANDLETYPE ap_hdl)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  assert (NULL != ap_obj);
  assert (NULL != ap_hdl);

  if (icer_con_get_listeners_count (ap_obj->p_server_) == 0)
    {
      return OMX_ErrorNone;
    }

  if (ap_obj->p_server_ && !ap_obj->server_is_full_)
    {
      rc = icer_con_write_to_listeners (ap_obj->p_server_, ap_hdl);

      switch (rc)
        {
        case OMX_ErrorNoMore:
          {
            /* Socket send buffers are full */
            ap_obj->server_is_full_ = true;
            TIZ_LOG_CNAME (TIZ_LOG_TRACE, TIZ_CNAME (ap_hdl),
                           TIZ_CBUF (ap_hdl), "[%s] : ", tiz_err_to_str (rc));
            rc = OMX_ErrorNone;
          }
          break;

        case OMX_ErrorOverflow:
          {
            /* Trying not send too much data. */
            ap_obj->server_is_full_ = false;
            TIZ_LOG_CNAME (TIZ_LOG_TRACE, TIZ_CNAME (ap_hdl),
                           TIZ_CBUF (ap_hdl), "[%s] : ", tiz_err_to_str (rc));
            rc = OMX_ErrorNone;
          }
          break;

        case OMX_ErrorNone:
          {
            /* More data needed */
            ap_obj->server_is_full_ = false;
            TIZ_LOG_CNAME (TIZ_LOG_TRACE, TIZ_CNAME (ap_hdl),
                           TIZ_CBUF (ap_hdl), "[%s] : ", tiz_err_to_str (rc));
          }
          break;

        case OMX_ErrorNotReady:
          {
            /* No connected clients yet */
            ap_obj->server_is_full_ = false;
            TIZ_LOG_CNAME (TIZ_LOG_TRACE, TIZ_CNAME (ap_hdl),
                           TIZ_CBUF (ap_hdl), "[%s] : ", tiz_err_to_str (rc));
            rc = OMX_ErrorNone;
          }
          break;

        default:
          {
            TIZ_LOG_CNAME (TIZ_LOG_TRACE, TIZ_CNAME (ap_hdl),
                           TIZ_CBUF (ap_hdl),
                           "[%s] : Error while writing to clients ...",
                           tiz_err_to_str (rc));
          }
          break;

        };
    }

  return rc;
}

static OMX_BUFFERHEADERTYPE *
buffer_needed (void *ap_arg)
{
  struct icerprc *p_obj = ap_arg;

  assert (NULL != p_obj);

  if (!(p_obj->eos_))
    {
      if (NULL != p_obj->p_inhdr_ && p_obj->p_inhdr_->nFilledLen > 0)
        {
          return p_obj->p_inhdr_;
        }
      else
        {
          const struct tizservant *p_parent = ap_arg;
          tiz_pd_set_t ports;
          void *p_krn = NULL;

          assert (false == p_obj->server_is_full_);

          assert (NULL != p_parent->p_hdl_);
          p_krn = tiz_get_krn (p_parent->p_hdl_);

          TIZ_PD_ZERO (&ports);
          if (OMX_ErrorNone == tiz_kernel_select (p_krn, 1, &ports))
            {
              if (TIZ_PD_ISSET (0, &ports))
                {
                  if (OMX_ErrorNone == tiz_kernel_claim_buffer
                      (p_krn, 0, 0, &p_obj->p_inhdr_))
                    {
                      TIZ_LOG (TIZ_LOG_TRACE, "Claimed HEADER [%p]...",
                               p_obj->p_inhdr_);
                      return p_obj->p_inhdr_;
                    }
                }
            }
        }
    }

  return NULL;
}

static void
buffer_emptied (OMX_BUFFERHEADERTYPE * p_hdr, void *p_arg)
{
  struct icerprc *p_obj = p_arg;
  const struct tizservant *p_parent = p_arg;
  void *p_krn = NULL;

  assert (NULL != p_obj);
  assert (NULL != p_hdr);
  assert (p_obj->p_inhdr_ == p_hdr);

  TIZ_LOG (TIZ_LOG_TRACE, "HEADER emptied [%p]", p_hdr);

  p_krn = tiz_get_krn (p_parent->p_hdl_);
  assert (NULL != p_krn);
  assert (p_hdr->nFilledLen == 0);

  p_obj->server_is_full_ = false;
  p_hdr->nOffset = 0;

  tiz_kernel_relinquish_buffer (p_krn, 0, p_hdr);
  p_obj->p_inhdr_ = NULL;
}

/*
 * icerprc
 */

static void *
icer_proc_ctor (void *ap_obj, va_list * app)
{
  struct icerprc *p_obj = super_ctor (icerprc, ap_obj, app);
  p_obj->bind_address_ = NULL;
  p_obj->lstn_port_ = 0;
  p_obj->mount_name_ = NULL;
  p_obj->max_clients_ = 0;
  p_obj->burst_size_ = 65536;
  p_obj->server_is_full_ = false;
  p_obj->eos_ = false;
  p_obj->lstn_sockfd_ = ICE_RENDERER_SOCK_ERROR;
  p_obj->p_server_ = NULL;
  p_obj->p_inhdr_ = NULL;
  return p_obj;
}

static void *
icer_proc_dtor (void *ap_obj)
{
  return super_dtor (icerprc, ap_obj);
}

/*
 * from tizservant class
 */

static OMX_ERRORTYPE
icer_proc_allocate_resources (void *ap_obj, OMX_U32 a_pid)
{
  struct icerprc *p_obj = ap_obj;
  const struct tizservant *p_parent = ap_obj;
  void *p_krn = NULL;
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  OMX_TIZONIA_PARAM_HTTPSERVERTYPE httpsrv;

  assert (NULL != ap_obj);
  assert (NULL != p_parent->p_hdl_);

  p_krn = tiz_get_krn (p_parent->p_hdl_);
  assert (NULL != p_krn);

  /* Retrieve http server configuration from the component's config port */
  httpsrv.nSize = sizeof (OMX_TIZONIA_PARAM_HTTPSERVERTYPE);
  httpsrv.nVersion.nVersion = OMX_VERSION;
  if (OMX_ErrorNone != (rc = tizapi_GetParameter (p_krn, p_parent->p_hdl_,
                                                  OMX_TizoniaIndexParamHttpServer,
                                                  &httpsrv)))
    {
      TIZ_LOG_CNAME (TIZ_LOG_TRACE, TIZ_CNAME (p_parent->p_hdl_),
                     TIZ_CBUF (p_parent->p_hdl_),
                     "[%s] : Error retrieving HTTPSERVERTYPE from port",
                     tiz_err_to_str (rc));
      return rc;
    }

  TIZ_LOG_CNAME (TIZ_LOG_TRACE, TIZ_CNAME (p_parent->p_hdl_),
                 TIZ_CBUF (p_parent->p_hdl_),
                 "nListeningPort = [%d] nMaxClients = [%d] ",
                 httpsrv.nListeningPort, httpsrv.nMaxClients);

  p_obj->lstn_port_ = httpsrv.nListeningPort;
  p_obj->max_clients_ = httpsrv.nMaxClients;

  if (OMX_ErrorNone != (rc = tiz_event_loop_init ()))
    {
      TIZ_LOG_CNAME (TIZ_LOG_TRACE, TIZ_CNAME (p_parent->p_hdl_),
                     TIZ_CBUF (p_parent->p_hdl_),
                     "[%s] : Error starting event loop", tiz_err_to_str (rc));
      return rc;
    }

  return icer_con_server_init (&(p_obj->p_server_), p_parent->p_hdl_,
                               p_obj->bind_address_, p_obj->lstn_port_,
                               p_obj->max_clients_, buffer_emptied,
                               buffer_needed, p_obj);
}

static OMX_ERRORTYPE
icer_proc_deallocate_resources (void *ap_obj)
{
  struct icerprc *p_obj = ap_obj;
  const struct tizservant *p_parent = ap_obj;

  assert (NULL != ap_obj);
  assert (NULL != p_parent->p_hdl_);

  icer_con_server_destroy (p_obj->p_server_, p_parent->p_hdl_);
  p_obj->p_server_ = NULL;
  tiz_event_loop_destroy ();

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
icer_proc_prepare_to_transfer (void *ap_obj, OMX_U32 a_pid)
{
  struct icerprc *p_obj = ap_obj;
  const struct tizservant *p_parent = ap_obj;

  assert (NULL != ap_obj);
  assert (NULL != p_parent->p_hdl_);

  TIZ_LOG (TIZ_LOG_TRACE,
           "Server starts listening on port [%d]", p_obj->lstn_port_);

  p_obj->lstn_sockfd_ = icer_con_get_server_fd (p_obj->p_server_);

  return icer_con_start_listening (p_obj->p_server_, p_parent->p_hdl_);
}

static OMX_ERRORTYPE
icer_proc_transfer_and_process (void *ap_obj, OMX_U32 a_pid)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
icer_proc_stop_and_return (void *ap_obj)
{
  struct icerprc *p_obj = ap_obj;
  const struct tizservant *p_parent = ap_obj;

  assert (NULL != ap_obj);

  TIZ_LOG (TIZ_LOG_TRACE, "Stopped buffer transfer...p_obj = [%p]", p_obj);

  return icer_con_stop_listening (p_obj->p_server_, p_parent->p_hdl_);
}

/*
 * from tizproc class
 */

static OMX_ERRORTYPE
icer_proc_buffers_ready (const void *ap_obj)
{
  struct icerprc *p_obj = (struct icerprc *) ap_obj;
  const struct tizservant *p_parent = ap_obj;

  assert (NULL != p_parent);
  assert (NULL != p_parent->p_hdl_);

  return stream_to_clients (p_obj, p_parent->p_hdl_);
}

static OMX_ERRORTYPE
icer_event_io_ready (void *ap_obj,
                     tiz_event_io_t * ap_ev_io, int a_fd, int a_events)
{
  struct icerprc *p_obj = ap_obj;
  struct tizservant *p_parent = ap_obj;
  OMX_HANDLETYPE p_hdl = NULL;
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  assert (NULL != p_obj);
  assert (NULL != p_parent->p_hdl_);
  p_hdl = p_parent->p_hdl_;

  TIZ_LOG_CNAME (TIZ_LOG_TRACE, TIZ_CNAME (p_hdl), TIZ_CBUF (p_hdl),
                 "Received io event on socket fd [%d] lstn_sockfd_ [%d]",
                 a_fd, p_obj->lstn_sockfd_);

  if (a_fd == p_obj->lstn_sockfd_)
    {
      rc = icer_con_accept_connection (p_obj->p_server_, p_hdl);

      if (OMX_ErrorInsufficientResources != rc)
        {
          rc = OMX_ErrorNone;
        }
    }
  else
    {
      if (a_events & TIZ_EVENT_WRITE)
        {
          p_obj->server_is_full_ = false;
        }

      rc = stream_to_clients (p_obj, p_hdl);
    }

  return rc;
}

static OMX_ERRORTYPE
icer_event_timer_ready (void *ap_obj, tiz_event_timer_t * ap_ev_timer,
                        void *ap_arg)
{
  struct icerprc *p_obj = ap_obj;
  struct tizservant *p_parent = ap_obj;
  OMX_HANDLETYPE p_hdl = NULL;

  assert (NULL != p_obj);
  assert (NULL != p_parent->p_hdl_);
  p_hdl = p_parent->p_hdl_;

  TIZ_LOG_CNAME (TIZ_LOG_NOTICE, TIZ_CNAME (p_hdl), TIZ_CBUF (p_hdl),
                 "Received timer event ");
  fflush (stdout);

  p_obj->server_is_full_ = false;

  return stream_to_clients (p_obj, p_hdl);;
}

/*
 * initialization
 */

const void *icerprc;

void
init_icerprc (void)
{
  if (!icerprc)
    {
      init_tizproc ();
      icerprc =
        factory_new
        (tiz_proc_class,
         "icerprc",
         tizproc,
         sizeof (struct icerprc),
         ctor, icer_proc_ctor,
         dtor, icer_proc_dtor,
         tiz_proc_buffers_ready, icer_proc_buffers_ready,
         tiz_servant_allocate_resources, icer_proc_allocate_resources,
         tiz_servant_deallocate_resources, icer_proc_deallocate_resources,
         tiz_servant_prepare_to_transfer, icer_proc_prepare_to_transfer,
         tiz_servant_transfer_and_process, icer_proc_transfer_and_process,
         tiz_servant_stop_and_return, icer_proc_stop_and_return,
         tiz_proc_event_io_ready, icer_event_io_ready,
         tiz_proc_event_timer_ready, icer_event_timer_ready, 0);
    }
}
