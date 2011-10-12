//
// Copyright (C) 2010 Codership Oy <info@codership.com>
//


#include "replicator_smm.hpp"
#include "uuid.hpp"
#include <galerautils.hpp>

namespace galera {

wsrep_status_t
ReplicatorSMM::sst_received(const wsrep_uuid_t& uuid,
                            wsrep_seqno_t       seqno,
                            const void*         state,
                            size_t              state_len)
{
    log_info << "Received SST: " << uuid << ':' << seqno;

    if (state_() != S_JOINING)
    {
        log_error << "not JOINING when sst_received() called, state: "
                  << state_();
        return WSREP_CONN_FAIL;
    }

    gu::Lock lock(sst_mutex_);

    sst_uuid_  = uuid;
    sst_seqno_ = seqno;
    sst_cond_.signal();

    return WSREP_OK;
}


class StateRequest_v0 : public ReplicatorSMM::StateRequest
{
public:
    StateRequest_v0 (const void* const sst_req, ssize_t const sst_req_len)
        : req_(sst_req), len_(sst_req_len)
    {}
    ~StateRequest_v0 () {}
    virtual const void* req     () const { return req_; }
    virtual ssize_t     len     () const { return len_; }
    virtual const void* sst_req () const { return req_; }
    virtual ssize_t     sst_len () const { return len_; }
    virtual const void* ist_req () const { return 0;    }
    virtual ssize_t     ist_len () const { return 0;    }
private:
    StateRequest_v0 (const StateRequest_v0&);
    StateRequest_v0& operator = (const StateRequest_v0&);
    const void* const req_;
    ssize_t     const len_;
};


class StateRequest_v1 : public ReplicatorSMM::StateRequest
{
public:
    static std::string const MAGIC;
    StateRequest_v1 (const void* sst_req, ssize_t sst_req_len,
                     const void* ist_req, ssize_t ist_req_len)
        throw (gu::Exception);
    StateRequest_v1 (const void* str, ssize_t str_len) throw (gu::Exception); 
    ~StateRequest_v1 () { if (own_ && req_) free (req_); }
    virtual const void* req     () const { return req_; }
    virtual ssize_t     len     () const { return len_; }
    virtual const void* sst_req () const { return req(sst_offset()); }
    virtual ssize_t     sst_len () const { return len(sst_offset()); }
    virtual const void* ist_req () const { return req(ist_offset()); }
    virtual ssize_t     ist_len () const { return len(ist_offset()); }
private:
    StateRequest_v1 (const StateRequest_v1&);
    StateRequest_v1& operator = (const StateRequest_v1&);

    ssize_t sst_offset() const { return MAGIC.length() + 1; }
    ssize_t ist_offset() const
    {
        return sst_offset() + sizeof(uint32_t) + sst_len();
    }

    ssize_t len (ssize_t offset) const
    {
        return gtohl(*(reinterpret_cast<uint32_t*>(req_ + offset)));
    }

    void*   req (ssize_t offset) const
    {
        if (len(offset) > 0)
            return req_ + offset + sizeof(uint32_t);
        else
            return 0;
    }

    ssize_t const len_;
    char*   const req_;
    bool    const own_;
};

std::string const
StateRequest_v1::MAGIC("STRv1");

#ifndef INT32_MAX
#define INT32_MAX 0x7fffffff
#endif

StateRequest_v1::StateRequest_v1 (
    const void* const sst_req, ssize_t const sst_req_len,
    const void* const ist_req, ssize_t const ist_req_len) throw (gu::Exception)
    :
    len_(MAGIC.length() + 1 +
         sizeof(uint32_t) + sst_req_len +
         sizeof(uint32_t) + ist_req_len),
    req_(reinterpret_cast<char*>(malloc(len_))),
    own_(true)
{
    if (!req_)
        gu_throw_error (ENOMEM) << "Could not allocate state request v1";

    if (sst_req_len > INT32_MAX || sst_req_len < 0)
        gu_throw_error (EMSGSIZE) << "SST request length (" << sst_req_len
                               << ") unrepresentable";

    if (ist_req_len > INT32_MAX || ist_req_len < 0)
        gu_throw_error (EMSGSIZE) << "IST request length (" << sst_req_len
                               << ") unrepresentable";

    char* ptr(req_);

    strcpy (ptr, MAGIC.c_str());
    ptr += MAGIC.length() + 1;

    uint32_t* tmp(reinterpret_cast<uint32_t*>(ptr));
    *tmp = htogl(sst_req_len);
    ptr += sizeof(uint32_t);

    memcpy (ptr, sst_req, sst_req_len);
    ptr += sst_req_len;

    tmp = reinterpret_cast<uint32_t*>(ptr);
    *tmp = htogl(ist_req_len);
    ptr += sizeof(uint32_t);

    memcpy (ptr, ist_req, ist_req_len);

    assert ((ptr - req_) == (len_ - ist_req_len));
}

// takes ownership over str buffer
StateRequest_v1::StateRequest_v1 (const void* str, ssize_t str_len)
    throw (gu::Exception)
:
    len_(str_len),
    req_(reinterpret_cast<char*>(const_cast<void*>(str))),
    own_(false)
{
    if (sst_offset() + 2*sizeof(uint32_t) > size_t(len_))
    {
        assert(0);
        gu_throw_error (EINVAL) << "State transfer request is too short: "
                                << len_ << ", must be at least: "
                                << (sst_offset() + 2*sizeof(uint32_t));
    }

    if (strncmp (req_, MAGIC.c_str(), MAGIC.length()))
    {
        assert(0);
        gu_throw_error (EINVAL) << "Wrong magic signature in state request v1.";
    }

    if (sst_offset() + sst_len() + 2*sizeof(uint32_t) > size_t(len_))
    {
        gu_throw_error (EINVAL) << "Malformed state request v1: sst length: "
                                << sst_len() << ", total length: " << len_;
    }

    if (ist_offset() + ist_len() + sizeof(uint32_t) != size_t(len_))
    {
        gu_throw_error (EINVAL) << "Malformed state request v1: parsed field "
            "length " << sst_len() << " is not equal to total request length "
                                << len_;
    }
}


static ReplicatorSMM::StateRequest*
read_state_request (const void* const req, size_t const req_len)
    throw (gu::Exception)
{
    const char* const str(reinterpret_cast<const char*>(req));

    if (req_len > StateRequest_v1::MAGIC.length() &&
        !strncmp(str, StateRequest_v1::MAGIC.c_str(),
                 StateRequest_v1::MAGIC.length()))
    {
        return (new StateRequest_v1(req, req_len));
    }
    else
    {
        return (new StateRequest_v0(req, req_len));
    }
}


class IST_request
{
public:
    IST_request() : peer_(), uuid_(), last_applied_(), group_seqno_() { }
    IST_request(const std::string& peer,
                const wsrep_uuid_t& uuid,
                wsrep_seqno_t last_applied,
                wsrep_seqno_t group_seqno)
        :
        peer_(peer),
        uuid_(uuid),
        last_applied_(last_applied),
        group_seqno_(group_seqno)
    { }
    const std::string&  peer()  const { return peer_ ; }
    const wsrep_uuid_t& uuid()  const { return uuid_ ; }
    wsrep_seqno_t       last_applied() const { return last_applied_; }
    wsrep_seqno_t       group_seqno()  const { return group_seqno_; }
private:
    friend std::ostream& operator<<(std::ostream&, const IST_request&);
    friend std::istream& operator>>(std::istream&, IST_request&);
    std::string peer_;
    wsrep_uuid_t uuid_;
    wsrep_seqno_t last_applied_;
    wsrep_seqno_t group_seqno_;
};

std::ostream& operator<<(std::ostream& os, const IST_request& istr)
{
    return (os
            << istr.uuid_ << ":"
            << istr.last_applied_ << ":"
            << istr.group_seqno_ << ":"
            << istr.peer_);
}

std::istream& operator>>(std::istream& is, IST_request& istr)
{
    char c;
    return (is >> istr.uuid_ >> c >> istr.last_applied_
            >> c >> istr.group_seqno_ >> c >> istr.peer_);
}

static void
serve_IST (gcache::GCache& gcache, const IST_request& istr) throw() // stub
{
    // char* const ist(strndup (reinterpret_cast<const char*>(req), len));
    // log_info << "Got IST request: '" << ist << '\'';
    // free (ist);
    log_info << "serving IST: " << istr;
    try
    {
        galera::ist::Sender ist_sender(gcache, istr.peer());
        ist_sender.send(istr.last_applied() + 1, istr.group_seqno());
    }
    catch (asio::system_error& e)
    {
        log_error << "IST serve failed: " << e.what();
    }
    catch (gu::Exception& e)
    {
        log_error << "IST serve failed: " << e.what();
    }
}

void ReplicatorSMM::process_state_req(void*       recv_ctx,
                                      const void* req,
                                      size_t      req_size,
                                      wsrep_seqno_t const seqno_l,
                                      wsrep_seqno_t const donor_seq)
    throw (gu::Exception)
{
    assert(recv_ctx != 0);
    assert(seqno_l > -1);
    assert(req != 0);

    LocalOrder lo(seqno_l);

    gu_trace(local_monitor_.enter(lo));
    apply_monitor_.drain(donor_seq);

    if (co_mode_ != CommitOrder::BYPASS) commit_monitor_.drain(donor_seq);

    state_.shift_to(S_DONOR);

    // somehow the following does not work, string is initialized beyond
    // the first \0:
    // std::string const req_str(reinterpret_cast<const char*>(req), req_size);
    // have to resort to C ways.

    char* const tmp(strndup(reinterpret_cast<const char*>(req), req_size));
    std::string const req_str(tmp);
    free (tmp);
    bool const trivial_sst(req_str == TRIVIAL_SST);

    if (!trivial_sst)
    {
        StateRequest* const streq (read_state_request (req, req_size));
        if (streq->ist_len())
        {
            IST_request istr;
            std::string ist_str(reinterpret_cast<const char*>(streq->ist_req()),
                                streq->ist_len());
            std::istringstream is(ist_str);
            is >> istr;
            if (istr.uuid() == state_uuid_)
            {
                log_info << "IST request: " << istr;
                try
                {
                    gcache_.seqno_lock(istr.last_applied() + 1);
                }
                catch(gu::NotFound& nf)
                {
                    log_info << "IST first seqno " << istr.last_applied() + 1
                             << " not found from cache, falling back to SST";
                    goto sst;
                }
                sst_donate_cb_(app_ctx_, recv_ctx,
                               streq->sst_req(), streq->sst_len(),
                               &istr.uuid(), istr.last_applied(), 0, 0, true);
                try
                {
                    serve_IST (gcache_, istr);
                }
                catch (gu::Exception& e)
                {
                    log_error << "failed to serve ist " << e.what();
                }
                goto end;
            }
        }

    sst:
        if (streq->sst_len())
        {
            sst_donate_cb_(app_ctx_, recv_ctx,
                           streq->sst_req(), streq->sst_len(),
                           &state_uuid_, donor_seq, 0, 0, false);
        }
    end:
        delete streq;
    }

    local_monitor_.leave(lo);

    if (trivial_sst)
    {
        gcs_.join(donor_seq);
    }
}


void
ReplicatorSMM::prepare_for_IST (void*& ptr, ssize_t& len,
                                wsrep_seqno_t group_seqno)
    throw (gu::Exception)
{
    std::ostringstream os;

//    os << "This is a mock IST request, starting position: " << state_uuid_
//       << ":" << apply_monitor_.last_left();
    os << IST_request(config_.get("ist.listen_addr"),
                      state_uuid_, apply_monitor_.last_left(), group_seqno);
    char* str = strdup (os.str().c_str());

    if (!str) gu_throw_error (ENOMEM) << "Failed to allocate IST buffer.";

    len = strlen(str) + 1;

    ptr = str;
}


ReplicatorSMM::StateRequest*
ReplicatorSMM::prepare_state_request (const void* const sst_req,
                                      ssize_t     const sst_req_len,
                                      wsrep_seqno_t group_seqno)
    throw()
{
    try
    {
        switch (str_proto_ver_)
        {
        case 0:
            return new StateRequest_v0 (sst_req, sst_req_len);
        case 1:
        {
            void*   ist_req;
            ssize_t ist_req_len;

            prepare_for_IST (ist_req, ist_req_len, group_seqno);

            return new StateRequest_v1 (sst_req, sst_req_len,
                                     ist_req, ist_req_len);
        }
        break;
        default:
            gu_throw_fatal << "Unsupported STR protocol: " << str_proto_ver_;
        }
    }
    catch (gu::Exception& e)
    {
        log_fatal << "State request preparation failed, aborting: " << e.what();
        abort();
    }

    throw;
}

static bool
retry_str(int ret)
{
    return (ret == -EAGAIN || ret == -ENOTCONN);
}


void
ReplicatorSMM::send_state_request (const wsrep_uuid_t&       group_uuid,
                                   wsrep_seqno_t const       group_seqno,
                                   const StateRequest* const req)
    throw ()
{
    long ret;
    long tries = 0;

    do
    {
        invalidate_state(state_file_);

        tries++;

        gcs_seqno_t seqno_l;

        ret = gcs_.request_state_transfer(req->req(), req->len(), sst_donor_,
                                          &seqno_l);

        if (ret < 0)
        {
            if (!retry_str(ret))
            {
                store_state(state_file_);
                log_error << "Requesting state transfer failed: "
                          << ret << "(" << strerror(-ret) << ")";
            }
            else if (1 == tries)
            {
                log_info << "Requesting state transfer failed: "
                         << ret << "(" << strerror(-ret) << "). "
                         << "Will keep retrying every " << sst_retry_sec_
                         << " second(s)";
            }
        }

        if (seqno_l != GCS_SEQNO_ILL)
        {
            /* Check that we're not running out of space in monitor. */
            if (local_monitor_.would_block(seqno_l))
            {
                long const seconds = sst_retry_sec_ * local_monitor_.size();
                double const hours = (seconds/360) * 0.1;
                log_error << "We ran out of resources, seemingly because "
                          << "we've been unsuccessfully requesting state "
                          << "transfer for over " << seconds << " seconds (>"
                          << hours << " hours). Please check that there is at "
                          << "least one fully synced member in the group. "
                          << "Application must be restarted.";
                ret = -EDEADLK;
            }
            else
            {
                // we are already holding local monitor
                LocalOrder lo(seqno_l);
                local_monitor_.self_cancel(lo);
            }
        }
    }
    while (retry_str(ret) && (usleep(sst_retry_sec_ * 1000000), true));

    if (ret >= 0)
    {
        if (1 == tries)
        {
            log_info << "Requesting state transfer: success, donor: " << ret;
        }
        else
        {
            log_info << "Requesting state transfer: success after "
                     << tries << " tries, donor: " << ret;
        }
    }
    else
    {
        sst_state_ = SST_REQ_FAILED;

        if (state_() > S_CLOSING)
        {
            log_fatal << "State transfer request failed unrecoverably: "
                      << -ret << " (" << strerror(-ret) << "). Most likely "
                      << "it is due to inability to communicate with the "
                      << "cluster primary component. Restart required.";
            abort();
        }
        else
        {
            // connection is being closed, send failure is expected
        }
    }
}


void
ReplicatorSMM::request_state_transfer (void* recv_ctx,
                                       const wsrep_uuid_t& group_uuid,
                                       wsrep_seqno_t const group_seqno,
                                       const void*   const sst_req,
                                       ssize_t       const sst_req_len)
    throw()
{
    assert(sst_req != 0);
    assert(sst_req_len > 0);

    StateRequest* const req(prepare_state_request(sst_req, sst_req_len,
                                                  group_seqno));

    log_debug << "State transfer required: "
              << "\n\tGroup state: "
              << group_uuid << ":" << group_seqno
              << "\n\tLocal state: " << state_uuid_
              << ":" << apply_monitor_.last_left();

    ist_receiver_.prepare();
    gu::Lock lock(sst_mutex_);

    send_state_request (group_uuid, group_seqno, req);

    state_.shift_to(S_JOINING);
    sst_state_ = SST_WAIT;

    /* while waiting for state transfer to complete is a good point
     * to reset gcache, since it may ivolve some IO too */
    gcache_.seqno_reset();

    lock.wait(sst_cond_);

    if (sst_uuid_ != group_uuid)
    {
        log_fatal << "Application received wrong state: "
                  << "\n\tReceived: " << sst_uuid_
                  << "\n\tRequired: " << group_uuid;
        sst_state_ = SST_FAILED;
        log_fatal << "Application state transfer failed. This is "
                  << "unrecoverable condition, restart required.";
        abort();
    }
    else
    {
        update_state_uuid (sst_uuid_);
        apply_monitor_.set_initial_position(-1);
        apply_monitor_.set_initial_position(sst_seqno_);

        if (co_mode_ != CommitOrder::BYPASS)
        {
            commit_monitor_.set_initial_position(-1);
            commit_monitor_.set_initial_position(sst_seqno_);
        }

        log_info << "SST finished: " << state_uuid_ << ":" << sst_seqno_;

        if (sst_seqno_ < group_seqno)
        {
            log_info << "Receiving IST: " << (group_seqno - sst_seqno_)
                     << " writesets.";
            // go to receive IST
            recv_IST(recv_ctx);
        }
    }
    ist_receiver_.finished();
    delete req;
}


void ReplicatorSMM::recv_IST(void* recv_ctx)
{
    while (true)
    {
        TrxHandle* trx(0);
        int err;
        if ((err = ist_receiver_.recv(&trx)) == 0)
        {
            assert(trx != 0);
            if (trx->depends_seqno() == -1)
            {
                ApplyOrder ao(*trx);
                apply_monitor_.self_cancel(ao);
                if (co_mode_ != CommitOrder::BYPASS)
                {
                    CommitOrder co(*trx, co_mode_);
                    commit_monitor_.self_cancel(co);
                }
            }
            else
            {
                apply_trx(recv_ctx, trx);
            }
            trx->unref();
        }
        else
        {
            return;
        }
    }
}


} /* namespace galera */


