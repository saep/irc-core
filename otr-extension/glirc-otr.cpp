#define _GNU_SOURCE

#include <cstring>
#include <cstdbool>
#include <cstdlib>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <tuple>

using namespace std;

extern "C" {
    #include <libotr/proto.h>
    #include <libotr/message.h>
    #include <libotr/privkey.h>
    #include "glirc-api.h"
}

#define NAME "OTR"
#define PLUGIN_USER "* OTR *"
#define MAJOR 1
#define MINOR 0

// IRC formatting escape sequences
#define PLAIN    "\17"
#define BOLD(x)  "\002"   x "\002"
#define GREEN(x) "\00303" x PLAIN
#define RED(x)   "\00304" x PLAIN

#define QUERY_TEXT "?OTRv23? This message is attempting to initiate an encrypted" \
                   " session, but your client doesn't support this protocol."

// These macros try to make it less error prone to marshal data
// into and out of the void *opdata parameters
#define GET_opdata auto opdata = static_cast<OpData*>(L)


static const char *casemap =
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
    "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
    " !\"#$%&'()*+,-./0123456789:;<=>?"
    "@abcdefghijklmnopqrstuvwxyz{|}~_"
    "`abcdefghijklmnopqrstuvwxyz{|}~\x7f"
    "\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f"
    "\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f"
    "\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf"
    "\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf"
    "\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf"
    "\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf"
    "\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef"
    "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff";


// IRC identifiers use a Swedish character encoding. The important
// distinction from normal ASCII is that "{|}~" are the lowercased
// forms of "[\]^". We normalize account names according to this
// convention so that OTR account names align with the meaning of
// IRC nicknames.
static void normalizeCase(string *str) {
   for (auto &x : *str) { x = casemap[(unsigned char)x]; }
}

/* Construct a glirc_string from a null-terminated C string */
static inline struct glirc_string mk_glirc_string(const char * str) {
        return (struct glirc_string) { .str = str, .len = strlen(str) };
}

/* Construct a C++ string from a glirc_string */
static string make_string(const glirc_string &s) {
        return string(s.str, s.len);
}


// It is useful to store a copy of the user state in the opdata
// because some callbacks forget to provide it
struct OpData {

    /* client API token */
    glirc *G;

    /* libotr session data */
    OtrlUserState us;

private:
    /* used to track open BATCHes by network */
    unordered_map<string, unordered_set<string>> batch_reftags;

public:
    OpData(glirc *G) : G(G), us(otrl_userstate_create()) {}
    ~OpData() { otrl_userstate_free(us); }

    tuple<string,string> current_focus() {

      char *net = NULL, *tgt = NULL;
      size_t netlen = 0, tgtlen = 0;

      glirc_current_focus(G, &net, &netlen, &tgt, &tgtlen);

      auto net_out = string(net, netlen);
      auto tgt_out = string(tgt, tgtlen);

      glirc_free_string(net);
      glirc_free_string(tgt);

      return make_tuple(net_out, tgt_out);
    }

    string my_nick(const string &network) {
        auto me = glirc_my_nick(G, network.c_str(), network.length());
        string result;
        if (me) {
                result = string(me);
                glirc_free_string(me);
        }
        return result;
    }

    ConnContext *get_current_context() {
        string net, tgt;
        tie(net,tgt) = current_focus();
        if (net.empty() || tgt.empty()) return NULL;
        normalizeCase(&tgt);

        auto me = my_nick(net);
        if (me.empty()) return NULL;
        normalizeCase(&me);

        return otrl_context_find
                (us, tgt.c_str(), me.c_str(), net.c_str(), OTRL_INSTAG_BEST, 0, NULL, NULL, NULL);
    }

    bool is_channel(const string &net, const string &tgt) {
        return glirc_is_channel(G, net.c_str(), net.length(), tgt.c_str(), tgt.length());
    }

    /* Populate the list of networks. This should run on startup in order to handle
     * the case where the extension is loaded while there are already connected
     * networks and the welcome message won't be seen
     */
    void populate_networks() {
        auto networks = glirc_list_networks(G);

        for (auto network = networks; *network; network++) {
            batch_reftags.insert(make_tuple(string(*network), unordered_set<string>()));
        }

        glirc_free_strings(networks);
    }

    /* BATCH logic */

    /* Initialize a network, resets the batch state */
    void add_network(const string &net) {
        batch_reftags.erase(net);
        batch_reftags.insert(make_tuple(net, unordered_set<string>()));
    }

    /* Start a batch for the given network tag and ref tag */
    void start_batch(const string &net, const string &reftag) {
        batch_reftags[net].insert(reftag);
    }

    /* End a batch for the given network tag and ref tag */
    void end_batch(const string &net, const string &reftag) {
        batch_reftags[net].erase(reftag);
    }

    /* Predicate to check if any batch is active on a particular network */
    bool in_batch(const string &net) {
        auto it = batch_reftags.find(net);
        return it != end(batch_reftags) && !it->second.empty();
    }
};

void glirc_vprintf (struct glirc *G, const char *net, const char *src, const char *tgt,
                    const char *fmt, va_list ap)
{
  char *msg = NULL;
  int len = vasprintf(&msg, fmt, ap);

  if (0 > len || !msg) abort();

  glirc_inject_chat
    (G, net, strlen(net),
     src, strlen(src),
     tgt, strlen(tgt),
     msg, len);

  free(msg);
}

static void glirc_printf(struct glirc *, const char *, const char *, const char *, const char *, ...)
__attribute__ ((format (printf, 5, 6)));

void glirc_printf (struct glirc *G, const char *net, const char *src,
                   const char *tgt, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  glirc_vprintf(G, net, src, tgt, fmt, ap);
  va_end(ap);
}

static void print_status(struct glirc *, ConnContext *, const char *, ...)
__attribute__ ((format (printf, 3, 4)));

void print_status(struct glirc *G, ConnContext *context, const char *fmt, ...)
{
  const char *net = context->protocol;
  const char *src = PLUGIN_USER;
  const char *tgt = context->username;

  va_list ap;
  va_start(ap, fmt);
  glirc_vprintf(G, net, src, tgt, fmt, ap);
  va_end(ap);
}


static char *state_path(const char *what)
{
  const char *home = getenv("HOME");
  char *res = NULL;
  if (home) {
    asprintf(&res, "%s/.config/glirc/otr-%s.txt", home, what);
  }
  return res;
}

static OtrlPolicy op_policy(void *L, ConnContext *context)
{
  (void)L; (void)context;

  return OTRL_POLICY_DEFAULT & ~OTRL_POLICY_SEND_WHITESPACE_TAG;
}

static void handle_smp_event
  (void *L, OtrlSMPEvent smp_event, ConnContext *context,
   unsigned short progress_percent, char *question)
{
  (void)progress_percent;
  GET_opdata;

  if (smp_event >= 9) return;

  const char *messages[9] = {};
  messages[OTRL_SMPEVENT_NONE          ] = BOLD("none");
  messages[OTRL_SMPEVENT_ERROR         ] = RED("error");
  messages[OTRL_SMPEVENT_ABORT         ] = RED("abort");
  messages[OTRL_SMPEVENT_CHEATED       ] = RED("cheated");
  messages[OTRL_SMPEVENT_ASK_FOR_ANSWER] = BOLD("question");
  messages[OTRL_SMPEVENT_ASK_FOR_SECRET] = BOLD("secret?");
  messages[OTRL_SMPEVENT_IN_PROGRESS   ] = BOLD("in progress");
  messages[OTRL_SMPEVENT_SUCCESS       ] = GREEN("success");
  messages[OTRL_SMPEVENT_FAILURE       ] = RED("failure");

  const char *message = messages[smp_event];

  if (question) {
    print_status(opdata->G, context, "Peer verification [%s] [%s]", message, question);
  } else {
    print_status(opdata->G, context, "Peer verification [%s]", message);
  }

  if (smp_event == OTRL_SMPEVENT_ASK_FOR_ANSWER || smp_event == OTRL_SMPEVENT_ASK_FOR_SECRET) {
    print_status(opdata->G, context, "Reply with: /extension " NAME " secret <answer>");
  }
}

static void
inject_message
  (void *L, const char *accountname,
  const char *protocol, const char *recipient, const char *message)
{
  (void)accountname;
  GET_opdata;

  OtrlMessageType msgtype = otrl_proto_message_type(message);

  // The default query message contains HTML and newlines!
  if (msgtype == OTRL_MSGTYPE_QUERY) {
    message = QUERY_TEXT;
  }

  struct glirc_string params[2] = {
    mk_glirc_string(recipient),
    mk_glirc_string(message),
  };

  struct glirc_message m = {
    .network  = mk_glirc_string(protocol),
    .command  = mk_glirc_string("PRIVMSG"),
    .params   = params,
    .params_n = 2,
  };

  glirc_send_message(opdata->G, &m);
}

static void
handle_msg_event
  (void *L, OtrlMessageEvent msg_event, ConnContext *context,
   const char *message, gcry_error_t err)
{
  (void)err;

  GET_opdata;

  static const char *messages[16] = {
    [OTRL_MSGEVENT_NONE                      ] = "None",
    [OTRL_MSGEVENT_ENCRYPTION_REQUIRED       ] = "Encryption required, message not sent",
    [OTRL_MSGEVENT_ENCRYPTION_ERROR          ] = "Encryption error, message not sent",
    [OTRL_MSGEVENT_CONNECTION_ENDED          ] = "Connection ended, message not sent",
    [OTRL_MSGEVENT_SETUP_ERROR               ] = "Setup error",
    [OTRL_MSGEVENT_MSG_REFLECTED             ] = "Message reflected",
    [OTRL_MSGEVENT_MSG_RESENT                ] = "Previous message resent",
    [OTRL_MSGEVENT_RCVDMSG_NOT_IN_PRIVATE    ] = "Received unexpected encrypted message",
    [OTRL_MSGEVENT_RCVDMSG_UNREADABLE        ] = "Received message unreadable",
    [OTRL_MSGEVENT_RCVDMSG_MALFORMED         ] = "Received message malformed",
    [OTRL_MSGEVENT_LOG_HEARTBEAT_RCVD        ] = NULL,
    [OTRL_MSGEVENT_LOG_HEARTBEAT_SENT        ] = NULL,
    [OTRL_MSGEVENT_RCVDMSG_GENERAL_ERR       ] = "Received general error",
    [OTRL_MSGEVENT_RCVDMSG_UNENCRYPTED       ] = "Received message unencrypted",
    [OTRL_MSGEVENT_RCVDMSG_UNRECOGNIZED      ] = "Received message unrecognizable",
    [OTRL_MSGEVENT_RCVDMSG_FOR_OTHER_INSTANCE] = "Received message for other instance", };

  const char *desc = messages[msg_event];

  if (desc) {
    if (message) {
      print_status(opdata->G, context, "%s [%s]", desc, message);
    } else {
      print_status(opdata->G, context, "%s", desc);
    }
  }
}

static int max_message_size(void *L, ConnContext *context)
{
  (void)L, (void)context;

  return 400; // pessimistic
}

static int is_logged_in
  (void *L, const char *accountname, const char *protocol,
   const char *recipient)
{
  (void)accountname;
  GET_opdata;

  int seen = glirc_is_logged_on(opdata->G,
                                protocol,  strlen(protocol),
                                recipient, strlen(recipient));

  return seen ? 1 : -1; // not seen just means we might not share a channel
}

static void gone_secure(void *L, ConnContext *context)
{
  GET_opdata;

  if (otrl_context_is_fingerprint_trusted(context->active_fingerprint)) {
    print_status(opdata->G, context, "Connection secured [" GREEN("trusted") "]");
  } else {
    print_status(opdata->G, context, "Connection secured [" RED("untrusted") "]");
  }
}

static void still_secure(void *L, ConnContext *context, int is_reply)
{
  GET_opdata;
  (void)is_reply;

  if (otrl_context_is_fingerprint_trusted(context->active_fingerprint)) {
    print_status(opdata->G, context, "Connection refreshed [" GREEN("trusted") "]");
  } else {
    print_status(opdata->G, context, "Connection refreshed [" RED("untrusted") "]");
  }
}

static void
new_fingerprint
  (void *L, OtrlUserState us, const char *accountname, const char *net,
   const char *tgt, unsigned char fp[20])
{
  (void)us, (void)accountname;
  GET_opdata;

  char human[OTRL_PRIVKEY_FPRINT_HUMAN_LEN];
  otrl_privkey_hash_to_human(human, fp);

  glirc_printf(opdata->G, net, PLUGIN_USER, tgt, "New fingerprint: [" BOLD("%s") "]", human);
}

static void write_fingerprints(void *L)
{
  GET_opdata;

  char *path = state_path("fingerprints");
  if (path) otrl_privkey_write_fingerprints(opdata->us, path);
  free(path);
}

static void create_privkey(void *L, const char *accountname, const char *protocol)
{
    GET_opdata;

    char *path = state_path("keys");
    if (path) otrl_privkey_generate(opdata->us, path, accountname, protocol);
    free(path);
}


static void create_instag(void *L, const char *accountname, const char *protocol)
{
    GET_opdata;

    char *path = state_path("instags");
    if (path) otrl_instag_generate(opdata->us, path, accountname, protocol);
    free(path);
}

static OtrlMessageAppOps ops = {
    .policy            = op_policy,
    .inject_message    = inject_message,
    .max_message_size  = max_message_size,
    .handle_msg_event  = handle_msg_event,
    .is_logged_in      = is_logged_in,

    .gone_secure       = gone_secure,
    .still_secure      = still_secure,
    .handle_smp_event  = handle_smp_event,

    .write_fingerprints = write_fingerprints,
    .new_fingerprint   = new_fingerprint,
    .create_privkey    = create_privkey,
    .create_instag     = create_instag,
};

static void *start_entrypoint(struct glirc *G, const char *libpath)
{
  (void)libpath;

  OTRL_INIT;
  auto opdata = new OpData(G);

  opdata->populate_networks();

  char *path = state_path("keys");
  if (path) otrl_privkey_read(opdata->us, path);
  free(path);

  path = state_path("fingerprints");
  if (path) otrl_privkey_read_fingerprints(opdata->us, path, NULL, NULL);
  free(path);

  path = state_path("instags");
  if (path) otrl_instag_read(opdata->us, path);
  free(path);

  return opdata;
}

static void stop_entrypoint(struct glirc *G, void *L)
{
  (void)G;
  GET_opdata;
  delete opdata;
}


// Rebuild the userinfo "nick!user@host" from the prefix
// fields of a glirc message
static string
rebuild_userinfo(const struct glirc_message *msg)
{
    ostringstream out;
    out << msg->prefix_nick.str;

    if (msg->prefix_user.len > 0) {
        out << "!" << msg->prefix_user.str;
    }

    if (msg->prefix_host.len > 0) {
        out << "@" << msg->prefix_host.str;
    }

    return out.str();
}

static enum process_result
process_welcome(OpData *opdata, const struct glirc_message *msg) {
        opdata->add_network(make_string(msg->network));
        return PASS_MESSAGE;
}

static enum process_result
process_batch(OpData *opdata, const struct glirc_message *msg) {

        if (msg->params_n < 1) return PASS_MESSAGE;

        auto reftag_ptr = msg->params[0].str;
        auto reftag = string(reftag_ptr+1, msg->params[0].len - 1);
        auto net = make_string(msg->network);

        switch (*reftag_ptr) {
                case '+':
                        opdata->start_batch(net, reftag);
                        break;
                case '-':
                        opdata->end_batch(net, reftag);
                        break;
        }

        return PASS_MESSAGE;
}

static enum process_result
process_privmsg(OpData *opdata, const struct glirc_message *msg)
{
    if (msg->params_n != 2) {
        return PASS_MESSAGE;
    }

    auto message = msg->params[1].str;
    auto net     = make_string(msg->network);
    auto target  = make_string(msg->params[0]);

    if (opdata->in_batch(net)) {
        return PASS_MESSAGE;
    }

    if (opdata->is_channel(net, target)) {
        return PASS_MESSAGE;
    }

    auto sender = make_string(msg->prefix_nick);
    normalizeCase(&sender);
    normalizeCase(&target);

    char *newmessage = NULL;

    int internal = otrl_message_receiving(opdata->us, &ops, opdata, target.c_str(), net.c_str(), sender.c_str(),
                      message, &newmessage, NULL, NULL, NULL, NULL);

    if (!internal && newmessage) {
        auto userinfo = rebuild_userinfo(msg);
        glirc_inject_chat(opdata->G,
                             msg->network.str    , msg->network.len,
                             userinfo.c_str()    , userinfo.length(),
                             msg->prefix_nick.str, msg->prefix_nick.len,
                             newmessage          , strlen(newmessage));
    }

    otrl_message_free(newmessage);

    return (internal || newmessage) ? DROP_MESSAGE : PASS_MESSAGE;
}

static enum process_result
message_entrypoint(struct glirc *G, void *L, const struct glirc_message *msg)
{
    GET_opdata;
    auto cmd = make_string(msg->command);

    if (cmd == "001") {
        return process_welcome(opdata, msg);
    } else if (cmd == "PRIVMSG") {
        return process_privmsg(opdata, msg);
    } else if (cmd == "BATCH") {
        return process_batch(opdata, msg);
    } else {
        return PASS_MESSAGE;
    }
}


static enum process_result chat_entrypoint(struct glirc *G, void *L, const struct glirc_chat *chat)
{
    (void)G;
    GET_opdata;

    gcry_error_t err = 1; // default to error unless sending runs and succeeds
    auto network = make_string(chat->network);
    auto target = make_string(chat->target);
    const char * msg = chat->message.str;

    if (opdata->is_channel(network, target)) {
        return PASS_MESSAGE;
    }

    auto me = opdata->my_nick(network);
    if (me.empty()) return PASS_MESSAGE;
    normalizeCase(&me);

    normalizeCase(&target);

    char * newmsg = NULL;

    err = otrl_message_sending
      (opdata->us, &ops, opdata, me.c_str(), network.c_str(), target.c_str(), OTRL_INSTAG_BEST, msg,
       NULL, &newmsg, OTRL_FRAGMENT_SEND_ALL, NULL, NULL, NULL);

    if (err) {
        glirc_printf(opdata->G, network.c_str(), PLUGIN_USER, target.c_str(), "PANIC: OTR encryption error");
    }

    otrl_message_free(newmsg);
    return err || newmsg ? DROP_MESSAGE : PASS_MESSAGE;
}

static void cmd_end (OpData *opdata, const string &params)
{
  (void)params;

  const char * const src = PLUGIN_USER;
  const char * const msg = RED("Session terminated");

  string net, tgt;
  tie(net,tgt) = opdata->current_focus();
  if (net.empty() || tgt.empty()) return;
  normalizeCase(&tgt);

  auto me = opdata->my_nick(net);
  if (me.empty()) return;
  normalizeCase(&me);

  otrl_message_disconnect_all_instances
          (opdata->us, &ops, opdata, me.c_str(), net.c_str(), tgt.c_str());

  glirc_inject_chat
    (opdata->G,
        net.c_str(), net.length(),
        src, strlen(src),
        tgt.c_str(), tgt.length(),
        msg, strlen(msg));
}


static void cmd_ask (OpData *opdata, const string &params)
{
    auto context = opdata->get_current_context();

    if (context) {
        otrl_message_initiate_smp
            (opdata->us, &ops, opdata, context,
             (unsigned char *)params.c_str(), params.length());
    }
}


static void cmd_secret (OpData *opdata, const string &params)
{
    auto context = opdata->get_current_context();

    // without this extra checking libotr will segfault if an exchange
    // is not active
    if (context && context->smstate && context->smstate->secret) {
        otrl_message_respond_smp
            (opdata->us, &ops, opdata, context,
             (unsigned char *)params.c_str(), params.length());
    }
}


static void cmd_poll (OpData *opdata, const string &params)
{
  (void)params;
  otrl_message_poll(opdata->us, &ops, opdata);
}


/*
 * Manually mark the fingerprint associated with the current window trusted.
 */
static void cmd_trust (OpData *opdata, const string &params)
{
  (void)params;

  auto context = opdata->get_current_context();
  if (!context || !context->active_fingerprint) return;

  otrl_context_set_trust(context->active_fingerprint, "manual");

  char *path = state_path("fingerprints");
  if (path) otrl_privkey_write_fingerprints(opdata->us, path);
  free(path);

  char human[OTRL_PRIVKEY_FPRINT_HUMAN_LEN];
  otrl_privkey_hash_to_human(human, context->active_fingerprint->fingerprint);
  print_status(opdata->G, context, "Fingerprint trusted [" BOLD("%s") "]", human);
}

/*
 * Manually mark the fingerprint associated with the current window untrusted.
 */
static void cmd_untrust (OpData *opdata, const string &params)
{
  (void)params;

  auto context = opdata->get_current_context();
  if (!context || !context->active_fingerprint) return;

  otrl_context_set_trust(context->active_fingerprint, "");

  char *path = state_path("fingerprints");
  if (path) otrl_privkey_write_fingerprints(opdata->us, path);
  free(path);

  char human[OTRL_PRIVKEY_FPRINT_HUMAN_LEN];
  otrl_privkey_hash_to_human(human, context->active_fingerprint->fingerprint);
  print_status(opdata->G, context, "Fingerprint untrusted [" BOLD("%s") "]", human);
}

/*
 * Print status information for the current context to the chat window
 */
static void cmd_status (OpData *opdata, const string &params)
{
  (void)params;

  auto context = opdata->get_current_context();
  if (!context) return;

  char human[OTRL_PRIVKEY_FPRINT_HUMAN_LEN];
  const char *myfp =
          otrl_privkey_fingerprint(opdata->us, human, context->accountname, context->protocol);

  if (myfp) {
    print_status(opdata->G, context, "Local  fingerprint [" BOLD("%s") "]", myfp);
  }

  Fingerprint *fp = context->active_fingerprint;
  if (fp) {
    otrl_privkey_hash_to_human(human, fp->fingerprint);

    if (otrl_context_is_fingerprint_trusted(fp)) {
      print_status(opdata->G, context, "Remote fingerprint [" BOLD("%s") "] [" GREEN("%s") "]", human, fp->trust);
    } else {
      print_status(opdata->G, context, "Remote fingerprint [" BOLD("%s") "] [" RED("untrusted") "]", human);
    }
  }

  print_status(opdata->G, context,
      "Local instance [" BOLD("%08X") "] Remote instance [" BOLD("%08X") "] Protocol [" BOLD("%u") "]",
      context->our_instance, context->their_instance, context->protocol_version);

  const char *statuses[] = {
    [OTRL_MSGSTATE_PLAINTEXT] = RED  ("plaintext"),
    [OTRL_MSGSTATE_ENCRYPTED] = GREEN("encrypted"),
    [OTRL_MSGSTATE_FINISHED ] = RED  ("finished" ),
  };

  print_status(opdata->G, context, "Connection state [%s]", statuses[context->msgstate]);
}

// Command metadata
struct cmd_impl {
  const char *name; // Name of command
  void (*func)(OpData*, const string &); // Implementation
  const char *doc; // Documentation string
};

static void cmd_help(OpData *, const string&);

static struct cmd_impl cmd_impls[] = {
  { "status" , cmd_status , "Display the current window's OTR context"              },
  { "secret" , cmd_secret , "Reply to a peer verification request (1 argument)"     },
  { "ask"    , cmd_ask    , "Send a peer verification request (1 argument)"         },
  { "end"    , cmd_end    , "Close the current window's OTR context"                },
  { "trust"  , cmd_trust  , "Trust the current remote user's fingerprint"           },
  { "untrust", cmd_untrust, "Revoke trust in the current remote user's fingerprint" },
  { "poll"   , cmd_poll   , "Manually trigger an OTR poll event"                    },
  { "help"   , cmd_help   , "Show available commands"                               },
};

#define ARRAY_LEN(x) (sizeof(x)/sizeof(*x))

static void cmd_help(OpData *opdata, const string &params)
{
  (void)params;

  for (int i = 0; i < ARRAY_LEN(cmd_impls); i++) {
    struct cmd_impl *c = cmd_impls+i;
    char *msg = NULL;
    int len = asprintf(&msg, "OTR: %s - %s", c->name, c->doc);
    if (len < 0 || !msg) abort();

    glirc_print(opdata->G, NORMAL_MESSAGE, msg, len);

    free(msg);
  }
}

static void command_entrypoint
  (struct glirc *G, void *L, const struct glirc_command *cmd)
{
  GET_opdata;
  const char *errmsg = "OTR: Unknown command";

  auto input = istringstream(make_string(cmd->command));
  string name, parameters;
  input >> name;
  getline(input, parameters);
  parameters.erase(0, parameters.find_first_not_of(" "));

  for (int i = 0; i < ARRAY_LEN(cmd_impls); i++) {
      struct cmd_impl *c = cmd_impls+i;
      if (name == c->name) {
          c->func(opdata, parameters);
          return;
      }
  }

  glirc_print(G, ERROR_MESSAGE, errmsg, strlen(errmsg));
}

struct glirc_extension extension = {
        .name            = NAME,
        .major_version   = MAJOR,
        .minor_version   = MINOR,
        .start           = start_entrypoint,
        .stop            = stop_entrypoint,
        .process_message = message_entrypoint,
        .process_chat    = chat_entrypoint,
        .process_command = command_entrypoint,
};
