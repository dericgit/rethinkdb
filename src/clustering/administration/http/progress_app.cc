#include "errors.hpp"
#include <boost/optional.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>

#include "arch/timing.hpp"
#include "clustering/administration/http/progress_app.hpp"
#include "clustering/administration/metadata.hpp"
#include "clustering/reactor/metadata.hpp"
#include "containers/uuid.hpp"
#include "http/json.hpp"
#include "http/json/json_adapter.hpp"

static const char * PROGRESS_REQ_TIMEOUT_PARAM = "timeout";
static const uint64_t DEFAULT_PROGRESS_REQ_TIMEOUT_MS = 500;
static const uint64_t MAX_PROGRESS_REQ_TIMEOUT_MS = 60*1000;

/* A record of a request made to another peer for progress on a backfill. */
class request_record_t {
public:
    promise_t<std::pair<int, int> > *promise;
    mailbox_t<void(std::pair<int, int>)> *resp_mbox;

    request_record_t(promise_t<std::pair<int, int> > *_promise, mailbox_t<void(std::pair<int, int>)> *_resp_mbox)
        : promise(_promise), resp_mbox(_resp_mbox)
    { }

    ~request_record_t() {
        delete promise;
        delete resp_mbox;
    }
};


/* Some typedefs to ostensibly make life suck less. */
typedef boost::ptr_map<memcached_protocol_t::region_t, request_record_t> region_to_request_record_t;
typedef std::map<reactor_activity_id_t, region_to_request_record_t> activity_id_map_t;
typedef std::map<namespace_id_t, activity_id_map_t> namespace_id_map_t;
typedef std::map<machine_id_t, namespace_id_map_t> machine_id_map_t;

/* A visitor that finds the backfiller business card (if it exists) in a
 * reactor activity. */
template <class protocol_t>
class get_backfiller_business_card_t : public boost::static_visitor<boost::optional<backfiller_business_card_t<protocol_t> > > {
public:
    boost::optional<backfiller_business_card_t<protocol_t> > operator()(const typename reactor_business_card_t<protocol_t>::primary_t &primary) const {
        if (primary.replier) {
            return primary.replier->backfiller_bcard;
        } else {
            return boost::optional<backfiller_business_card_t<protocol_t> >();
        }
    }

    boost::optional<backfiller_business_card_t<protocol_t> > operator()(const typename reactor_business_card_t<protocol_t>::primary_when_safe_t &) const {
        return boost::optional<backfiller_business_card_t<protocol_t> >();
    }


    boost::optional<backfiller_business_card_t<protocol_t> > operator()(const typename reactor_business_card_t<protocol_t>::secondary_up_to_date_t &secondary_up_to_date) const {
        return secondary_up_to_date.replier.backfiller_bcard;
    }

    boost::optional<backfiller_business_card_t<protocol_t> > operator()(const typename reactor_business_card_t<protocol_t>::secondary_without_primary_t &secondary_without_primary) const {
        return secondary_without_primary.backfiller;
    }

    boost::optional<backfiller_business_card_t<protocol_t> > operator()(const typename reactor_business_card_t<protocol_t>::secondary_backfilling_t &) const {
        return boost::optional<backfiller_business_card_t<protocol_t> >();
    }

    boost::optional<backfiller_business_card_t<protocol_t> > operator()(const typename reactor_business_card_t<protocol_t>::nothing_when_safe_t &nothing_when_safe) const {
        return nothing_when_safe.backfiller;
    }

    boost::optional<backfiller_business_card_t<protocol_t> > operator()(const typename reactor_business_card_t<protocol_t>::nothing_t &) const {
        return boost::optional<backfiller_business_card_t<protocol_t> >();
    }

    boost::optional<backfiller_business_card_t<protocol_t> > operator()(const typename reactor_business_card_t<protocol_t>::nothing_when_done_erasing_t &) const {
        return boost::optional<backfiller_business_card_t<protocol_t> >();
    }
};

/* A visitor to send out requests for the backfill progress of a reactor activity. */
class send_backfill_requests : public boost::static_visitor<void> {
public:
    send_backfill_requests(clone_ptr_t<directory_rview_t<cluster_directory_metadata_t> > _directory_metadata,
                           namespace_id_t _n_id,
                           machine_id_t _m_id,
                           reactor_activity_id_t _a_id,
                           memcached_protocol_t::region_t _region,
                           mailbox_manager_t *_mbox_manager,
                           machine_id_map_t *_promise_map)
        : directory_metadata(_directory_metadata),
          n_id(_n_id),
          m_id(_m_id),
          a_id(_a_id),
          region(_region),
          mbox_manager(_mbox_manager),
          promise_map(_promise_map)
    { }
    /* For most of the activities there is no backfill happening so we just do
     * a default visitation here that does nothing. */
    template <class T> 
    void operator()(const T &) const { }
private:
    clone_ptr_t<directory_rview_t<cluster_directory_metadata_t> > directory_metadata;
    namespace_id_t n_id;
    machine_id_t m_id;
    reactor_activity_id_t a_id;
    memcached_protocol_t::region_t region;
    mailbox_manager_t *mbox_manager;
    machine_id_map_t *promise_map;
};

template <>
void send_backfill_requests::operator()<reactor_business_card_t<memcached_protocol_t>::primary_when_safe_t>(const reactor_business_card_t<memcached_protocol_t>::primary_when_safe_t &primary_when_safe) const {
    for (std::vector<reactor_business_card_details::backfill_location_t>::const_iterator b_it  = primary_when_safe.backfills_waited_on.begin();
                                                                                         b_it != primary_when_safe.backfills_waited_on.end();
                                                                                         ++b_it) {
        namespaces_directory_metadata_t<memcached_protocol_t> namespaces_directory_metadata = directory_metadata->get_value(b_it->peer_id).get().memcached_namespaces;

        if (!std_contains(namespaces_directory_metadata.reactor_bcards, n_id)) {
            continue;
        }

        if (!std_contains(namespaces_directory_metadata.reactor_bcards[n_id].internal.activities, b_it->activity_id)) {
            continue;
        }

        std::pair<memcached_protocol_t::region_t, reactor_business_card_t<memcached_protocol_t>::activity_t> region_activity_pair = 
            namespaces_directory_metadata.reactor_bcards[n_id].internal.activities[b_it->activity_id];

        boost::optional<backfiller_business_card_t<memcached_protocol_t> > backfiller = boost::apply_visitor(get_backfiller_business_card_t<memcached_protocol_t>(), region_activity_pair.second);
        if (backfiller) {
            promise_t<std::pair<int, int> > *value = new promise_t<std::pair<int, int> >;
            mailbox_t<void(std::pair<int, int>)> *resp_mbox = new mailbox_t<void(std::pair<int, int>)>(mbox_manager, boost::bind(&promise_t<std::pair<int, int> >::pulse, value, _1));

            send(mbox_manager, backfiller->request_progress_mailbox, b_it->backfill_session_id, resp_mbox->get_address());

            rassert(!std_contains((*promise_map)[m_id][n_id][a_id], region));
            memcached_protocol_t::region_t tmp = region; /* gotta make a copy because of that boost ptr map bug */
            (*promise_map)[m_id][n_id][a_id].insert(tmp, new request_record_t(value, resp_mbox));
        } else {
            //cJSON_AddItemToObject(backfills, get_string(render_as_json(&region_activity_pair.first, 0)).c_str(), cJSON_CreateString("backfiller not found"));
        }
    }
}

template <>
void send_backfill_requests::operator()<reactor_business_card_t<memcached_protocol_t>::secondary_backfilling_t>(const reactor_business_card_t<memcached_protocol_t>::secondary_backfilling_t &secondary_backfilling) const {
    reactor_business_card_details::backfill_location_t b_loc = secondary_backfilling.backfill;

    namespaces_directory_metadata_t<memcached_protocol_t> namespaces_directory_metadata = directory_metadata->get_value(b_loc.peer_id).get().memcached_namespaces;

    if (!std_contains(namespaces_directory_metadata.reactor_bcards, n_id)) {
        return;
    }

    if (!std_contains(namespaces_directory_metadata.reactor_bcards[n_id].internal.activities, b_loc.activity_id)) {
        return;
    }

    std::pair<memcached_protocol_t::region_t, reactor_business_card_t<memcached_protocol_t>::activity_t> region_activity_pair = 
        namespaces_directory_metadata.reactor_bcards[n_id].internal.activities[b_loc.activity_id];

    boost::optional<backfiller_business_card_t<memcached_protocol_t> > backfiller = boost::apply_visitor(get_backfiller_business_card_t<memcached_protocol_t>(), region_activity_pair.second);
    if (backfiller) {
        promise_t<std::pair<int, int> > *value = new promise_t<std::pair<int, int> >;
        mailbox_t<void(std::pair<int, int>)> *resp_mbox = new mailbox_t<void(std::pair<int, int>)>(mbox_manager, boost::bind(&promise_t<std::pair<int, int> >::pulse, value, _1));

        send(mbox_manager, backfiller->request_progress_mailbox, b_loc.backfill_session_id, resp_mbox->get_address());

        rassert(!std_contains((*promise_map)[m_id][n_id][a_id], region));
        memcached_protocol_t::region_t tmp = region; /* fuck boost */
        (*promise_map)[m_id][n_id][a_id].insert(tmp, new request_record_t(value, resp_mbox));
    } else {
        //cJSON_AddItemToObject(backfills, get_string(render_as_json(&region_activity_pair.first, 0)).c_str(), cJSON_CreateString("backfiller not found"));
    }
}

static const char *any_machine_id_wildcard = "_";

//TODO why is this not const?
progress_app_t::progress_app_t(clone_ptr_t<directory_rview_t<cluster_directory_metadata_t> > _directory_metadata, mailbox_manager_t *_mbox_manager)
    : directory_metadata(_directory_metadata), mbox_manager(_mbox_manager)
{ }

http_res_t progress_app_t::handle(const http_req_t &req) {
    /* This function is an absolute mess, basically because we need to hack
     * through a mess of different data structures to find the various
     * backfills that are in progress and query the person serving the backfill
     * to get the data. There's really not much way for this to be nice that I
     * know of so I'd rather just have it all in one place. Fortunately there's
     * nothing too subtle going on here. */

    /* We need to assemble this big monolithic map with the following type (in shorthand):
     *
     * machine_id_t -> 
     *   namespace_id_t -> 
     *     reactor_activity_id_t -> 
     *       memcached_protocol_t::region_t -> 
     *         request_record_t
     *
     * A request record holds on to the mailbox needed to receive a value
     * from a peer. It also holds the promise needed to get that value when
     * the guy mails us back. */

    /* The actual map. The entire purpose of the block below is to put things
     * in this map. */
    machine_id_map_t promise_map;

    {
        /* First we create a scope for the peers_list_freeze_t, this makes sure
         * peers don't connect or disconnect so we want to get out of here
         * quickly. */
        connectivity_service_t *connectivity_service = directory_metadata->get_directory_service()->get_connectivity_service();
        connectivity_service_t::peers_list_freeze_t freeze_peers(connectivity_service);
        std::set<peer_id_t> peers_list = connectivity_service->get_peers_list();

        http_req_t::resource_t::iterator it = req.resource.begin();

        /* Check to see if we're only requesting the backfills happening on a
         * particular machine. */
        boost::optional<machine_id_t> requested_machine_id;
        if (it != req.resource.end()) {
            if (*it != any_machine_id_wildcard) {
                requested_machine_id = str_to_uuid(*it);
                if (requested_machine_id->is_nil()) {
                    throw schema_mismatch_exc_t(strprintf("Failed to parse %s as valid uuid\n", it->c_str()));
                }
            }
            ++it;
        }

        /* Check to see if we're only requesting the backfills happening for a
         * particular namespace. */
        boost::optional<namespace_id_t> requested_namespace_id;
        if (it != req.resource.end()) {
            if (*it != any_machine_id_wildcard) {
                requested_namespace_id = str_to_uuid(*it);
                if (requested_namespace_id->is_nil()) {
                    throw schema_mismatch_exc_t(strprintf("Failed to parse %s as valid uuid\n", it->c_str()));
                }
            }
            ++it;
        }


        /* Iterate through the peers. */
        for (std::set<peer_id_t>::iterator p_it  = peers_list.begin();
                p_it != peers_list.end();
                ++p_it) {
            /* Check to see if this matches the requested machine_id (or if we
             * didn't specify a specific machine but want all the machines). */
            if (!requested_machine_id || requested_machine_id == directory_metadata->get_value(*p_it).get().machine_id) {
                /* The machine id of the machine which is backfilling. */
                machine_id_t m_id = directory_metadata->get_value(*p_it).get().machine_id;

                typedef std::map<namespace_id_t, directory_echo_wrapper_t<reactor_business_card_t<memcached_protocol_t> > > reactor_bcard_map_t;
                reactor_bcard_map_t bcard_map = directory_metadata->get_value(*p_it).get().memcached_namespaces.reactor_bcards;

                /* Iterate through the machine's reactor's business_cards to see which ones are doing backfills. */
                for (std::map<namespace_id_t, directory_echo_wrapper_t<reactor_business_card_t<memcached_protocol_t> > >::iterator n_it  = bcard_map.begin();
                                                                                                                                   n_it != bcard_map.end();
                                                                                                                                   ++n_it) {
                    /* Check to see if this matches the requested namespace (or
                     * if we're just getting all the namespaces). */
                    if (!requested_namespace_id || requested_namespace_id == n_it->first) {

                        /* Iterate through the reactors activities to see if
                         * any of them are currently backfilling. */
                        for (reactor_business_card_t<memcached_protocol_t>::activity_map_t::iterator a_it  = n_it->second.internal.activities.begin();
                                                                                                     a_it != n_it->second.internal.activities.end();
                                                                                                     ++a_it) {
                            /* XXX we don't have a way to filter by activity
                             * id, there's no reason we couldn't but it doesn't
                             * seem like the ui has a use for it soe we're
                             * leaving it out. This could be a TODO. */

                            /* This visitor dispatches requests to the correct backfillers progress mailboxs. */
                            boost::apply_visitor(send_backfill_requests(directory_metadata, n_it->first, m_id, a_it->first,
                                                                        a_it->second.first, mbox_manager, &promise_map),
                                                 a_it->second.second);
                        }
                    }
                }
            }
        }
    }

    /* We've sent out request for all the progress reports. Now we need to
     * collect the results and put it in some json. */

    /* The json we'll be assembling things in to. */
    scoped_cJSON_t body(cJSON_CreateObject());

    /* If a machine has disconnected, or the mailbox for the
     * backfill has gone out of existence we'll never get a
     * response. Thus we need to have a time out.
     * We parse the 'timeout' query parameter here if it is
     * present, and if not, just use the default value of 500ms.
     */
    boost::optional<std::string> timeout_param = req.find_query_param(PROGRESS_REQ_TIMEOUT_PARAM);
    uint64_t timeout = DEFAULT_PROGRESS_REQ_TIMEOUT_MS;
    if (timeout_param) {
        if (!strtou64_strict(timeout_param.get(), 10, &timeout) || timeout == 0 || timeout > MAX_PROGRESS_REQ_TIMEOUT_MS) {
            http_res_t res(400);
            res.set_body("application/text", "Invalid timeout value");
            return res;
        }
    }

    signal_timer_t timer(timeout);

    /* Now we write a bunch of nested for loops to iterate through each layer,
     * this is annoying but hopefully it's pretty clear what's going on. */
    for (machine_id_map_t::iterator m_it  = promise_map.begin();
                                    m_it != promise_map.end();
                                    ++m_it) {
        cJSON *machine_info = cJSON_CreateObject();
        cJSON_AddItemToObject(body.get(), uuid_to_str(m_it->first).c_str(), machine_info);
        for (namespace_id_map_t::iterator n_it  = m_it->second.begin();
                                          n_it != m_it->second.end();
                                          ++n_it) {
            cJSON *namespace_info = cJSON_CreateObject();
            cJSON_AddItemToObject(machine_info, uuid_to_str(n_it->first).c_str(), namespace_info);
            for (activity_id_map_t::iterator a_it  = n_it->second.begin();
                                             a_it != n_it->second.end();
                                             ++a_it) {
                cJSON *activity_info = cJSON_CreateObject();
                cJSON_AddItemToObject(namespace_info, uuid_to_str(a_it->first).c_str(), activity_info);
                for (region_to_request_record_t::iterator r_it  = a_it->second.begin();
                                                          r_it != a_it->second.end();
                                                          ++r_it) {

                    //Sigh get around json adapters const aversion
                    memcached_protocol_t::region_t r = r_it->first;

                    /* Notice, once the timer has elapsed (possibly because
                     * someone timed out) all of these calls to wait will
                     * return immediately. That's okay though because we check
                     * that the promise is pulsed, not that the timer isn't So
                     * eacho request is guarunteed to get at least 500ms to
                     * complete. */
                    wait_any_t waiter(&timer, r_it->second->promise->get_ready_signal());
                    waiter.wait();

                    if (r_it->second->promise->get_ready_signal()->is_pulsed()) {
                        /* The promise is pulsed, we got an answer. */
                        std::pair<int, int> response = r_it->second->promise->wait();
                        cJSON *pair = cJSON_CreateArray();
                        cJSON_AddItemToArray(pair, cJSON_CreateNumber(response.first));
                        cJSON_AddItemToArray(pair, cJSON_CreateNumber(response.second));
                        cJSON_AddItemToObject(activity_info, get_string(render_as_json(&r, 0)).c_str(), pair);
                    } else {
                        /* The promise is not pulsed.. we timed out. */
                        cJSON_AddItemToObject(activity_info, get_string(render_as_json(&r, 0)).c_str(), cJSON_CreateString("Timeout"));
                    }
                }
            }
        }
    }

    http_res_t res(200);
    res.set_body("application/json", cJSON_print_std_string(body.get()));
    return res;
}