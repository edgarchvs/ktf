/*
 * Copyright (c) 2012, 2017, Oracle and/or its affiliates. All rights reserved.
 *    Author: Knut Omang <knut.omang@oracle.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 * ktf_int.cpp: Implementation of Gtest user land test management
 * for kernel and hybrid test functionality provided by KTF.
 */
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <unistd.h>
#include "kernel/ktf_unlproto.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include "ktf_int.h"
#include "ktf_debug.h"

#ifdef HAVE_LIBNL3
#include <netlink/version.h>
#else
#define nl_socket_alloc nl_handle_alloc
#define nl_socket_free nl_handle_destroy
#define nl_sock nl_handle
#endif

extern "C"
{
  /* From unlproto.c */
  struct nla_policy *get_ktf_gnl_policy();
}

int devcnt = 0;

namespace ktf
{

struct nl_sock* sock = NULL;
int family = -1;

int printed_header = 0;

typedef std::map<std::string, KernelTest*> testmap;
typedef std::map<std::string, test_cb*> wrappermap;

class testset
{
public:
  testset() : setnum(0)
  { }

  ~testset()
  {
    for (testmap::iterator it = tests.begin(); it != tests.end(); ++it)
      delete it->second;
  }

  testmap tests;
  stringvec test_names;
  wrappermap wrapper;
  int setnum;
};

/* Keeps track of a ktf_context that requires configuration.
 * Context names are unique within a handle, so handle ID is
 * necessary to identify the context.
 * The actual configuration data must be agreed upon between
 * user mode and kernel mode on a per context basis.
 * They can use type_id to identify which type of parameter a
 * context needs.
 */
class ConfigurableContext
{
public:
  ConfigurableContext(std::string& name_, unsigned type_id_, unsigned int hid, int cfg_stat_)
    : name(name_),
      handle_id(hid),
      type_id(type_id_),
      cfg_stat(cfg_stat_)
  {
    log(KTF_INFO, "%s[%u] (hid %d): state: %s\n",
	name.c_str(), type_id, hid, str_state().c_str());
  }

  std::string str_state()
  {
    switch (cfg_stat) {
    case 0:
      return std::string("READY");
    case ENOENT:
      return std::string("UNCONFIGURED");
    default:
      char tmp[100];
      sprintf(tmp, "ERROR(%d)", cfg_stat);
      return std::string(tmp);
    }
  }

  int Configure(void *data, size_t data_sz)
  {
    struct nl_msg *msg = nlmsg_alloc();
    int err;

    log(KTF_INFO, "%s, data_sz %lu\n", name.c_str(), data_sz);
    genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0, NLM_F_REQUEST,
		KTF_C_REQ, 1);
    nla_put_u32(msg, KTF_A_TYPE, KTF_CT_CTX_CFG);
    nla_put_u64(msg, KTF_A_VERSION, KTF_VERSION_LATEST);
    nla_put_string(msg, KTF_A_STR, name.c_str());
    nla_put_u32(msg, KTF_A_HID, handle_id);
    nla_put(msg, KTF_A_DATA, data_sz, data);

    // Send message over netlink socket
    nl_send_auto_complete(sock, msg);

    // Free message
    nlmsg_free(msg);

    // Wait for acknowledgement:
    // This function also returns error status if the message
    // was not deemed ok by the kernel, but the error status
    // does not resemble what the netlink recipient returned.
    //
    // This message receives no response beyond the error code.
    //
    err = nl_wait_for_ack(sock);
    return err;
  }

  unsigned int Type()
  {
    return type_id;
  }

  std::string name;
  int handle_id;
  int type_id;
  int cfg_stat;
};

typedef std::map<std::string, testset> setmap;
typedef std::set<std::string> stringset;
typedef std::vector<ConfigurableContext*> context_vector;

struct name_iter
{
  setmap::iterator it;
  std::string setname;
};


/* We trick the gtest template framework
 * to get a new set of test names as a side effect of
 * invocation of get_test_names()
 */

/* Wrap globals in an object to control init order and
 * memory cleanup:
 */
class KernelTestMgr
{
public:
  KernelTestMgr() : next_set(0), cur(NULL)
  { }

  ~KernelTestMgr();

  testset& find_add_set(std::string& setname);
  testset& find_add_test(std::string& setname, std::string& testname);
  void add_test(const std::string& setname, const char* tname, unsigned int handle_id);
  KernelTest* find_test(const std::string&setname, const std::string& testname, std::string* ctx);
  void add_wrapper(const std::string setname, const std::string testname, test_cb* tcb);

  stringvec& get_set_names() { return set_names; }
  stringvec get_test_names();

  stringvec get_testsets()
  {
    return set_names;
  }

  std::string get_current_setname()
  {
    return cur->setname;
  }

  stringvec& get_contexts(unsigned int id)
  {
    return handle_to_ctxvec[id];
  }

  void add_cset(unsigned int hid, stringvec& ctxs);
  void add_configurable_context(std::string& ctx, unsigned int type_id,
				unsigned int hid, int cfg_stat);
  std::vector<ConfigurableContext*> find_contexts(const std::string& ctx);
private:
  setmap sets;
  stringvec test_names;
  stringvec set_names;
  stringset kernelsets;
  std::map<unsigned int, stringvec> handle_to_ctxvec;
  std::map<std::string, context_vector> cfg_contexts;
  int next_set;
  name_iter* cur;
};

KernelTestMgr::~KernelTestMgr()
{
  std::map<std::string, context_vector>::iterator it;
  for (it = cfg_contexts.begin(); it != cfg_contexts.end(); ++it)
  {
    context_vector::iterator vit;
    for (vit = it->second.begin(); vit != it->second.end(); ++vit)
      delete *vit;
  }
}

context_vector KernelTestMgr::find_contexts(const std::string& ctx)
{
  std::map<std::string,context_vector>::iterator it;
  it = cfg_contexts.find(ctx);
  if (it == cfg_contexts.end())
    return context_vector();
  else
    return it->second;
}

KernelTestMgr& kmgr()
{
  static KernelTestMgr kmgr_;
  return kmgr_;
}

testset& KernelTestMgr::find_add_test(std::string& setname, std::string& testname)
{
  testset& ts(find_add_set(setname));
  test_names.push_back(testname);
  return ts;
}

testset& KernelTestMgr::find_add_set(std::string& setname)
{
  bool new_set = false;

  log(KTF_DEBUG, "find_add_set(%s)\n", setname.c_str());

  stringset::iterator it = kernelsets.find(setname);
  if (it == kernelsets.end()) {
    kernelsets.insert(setname);
    set_names.push_back(setname);
    new_set = true;
  }

  /* This implicitly adds a new testset to sets, if it's not there: */
  testset& ts = sets[setname];
  if (new_set)
  {
    ts.setnum = next_set++;
    log(KTF_INFO, "added %s (set %d) total %lu sets\n", setname.c_str(), ts.setnum, sets.size());
  }
  return ts;
}


void KernelTestMgr::add_test(const std::string& setname, const char* tname,
			     unsigned int handle_id)
{
  log(KTF_INFO_V, "add_test: %s.%s", setname.c_str(),tname);
  logs(KTF_INFO_V,
       if (handle_id)
	 fprintf(stderr, " [id %d]\n", handle_id);
       else
	 fprintf(stderr, "\n"));
  std::string name(tname);
  new KernelTest(setname, tname, handle_id);
}


/* Here we might get called with test names expanded with context names */
KernelTest* KernelTestMgr::find_test(const std::string&setname,
				     const std::string& testname,
				     std::string* pctx)
{
  size_t pos;
  log(KTF_DEBUG, "find test %s.%s\n", setname.c_str(), testname.c_str());

  /* Try direct lookup first: */
  KernelTest* kt = sets[setname].tests[testname];
  if (kt) {
    *pctx = std::string();
    return kt;
  }

  /* If we don't have any contexts set, no need to parse name: */
  if (handle_to_ctxvec.empty())
    return NULL;

  pos = testname.find_last_of('_');
  while (pos >= 0) {
    std::string tname = testname.substr(0,pos);
    std::string ctx = testname.substr(pos + 1, testname.npos);
    *pctx = ctx;
    kt = sets[setname].tests[tname];
    if (kt)
      return kt;
    /* context name might contain an '_' , iterate on: */
    pos = tname.find_last_of('_');
  }
  return NULL;
}


void KernelTestMgr::add_cset(unsigned int hid, stringvec& ctxs)
{
  log(KTF_INFO, "hid %d: ", hid);
  logs(KTF_INFO, for (stringvec::iterator it = ctxs.begin(); it != ctxs.end(); ++it)
	 fprintf(stderr, "%s ", it->c_str());
       fprintf(stderr, "\n"));
  handle_to_ctxvec[hid] = ctxs;
}

void KernelTestMgr::add_configurable_context(std::string& ctx, unsigned int type_id,
					     unsigned int hid, int cfg_stat)
{
  cfg_contexts[ctx].push_back(new ConfigurableContext(ctx, type_id, hid, cfg_stat));
}

/* Function for adding a wrapper user level test */
void KernelTestMgr::add_wrapper(const std::string setname, const std::string testname,
				test_cb* tcb)
{
  log(KTF_DEBUG, "add_wrapper: %s.%s\n", setname.c_str(),testname.c_str());
  testset& ts = sets[setname];

  /* Depending on C++ initialization order which vary between compiler version
   * (sigh!) either the kernel tests have already been processed or we have to store
   * this object in wrapper for later insertion:
   */
  KernelTest *kt = ts.tests[testname];
  if (kt) {
    log(KTF_DEBUG_V, "Assigning user_test for %s.%s\n",
	setname.c_str(), testname.c_str());
    kt->user_test = tcb;
  } else {
    log(KTF_DEBUG_V, "Set wrapper for %s.%s\n",
	setname.c_str(), testname.c_str());
    ts.wrapper[testname] = tcb;
  }
}


stringvec KernelTestMgr::get_test_names()
{
  if (!cur) {
    cur = new name_iter();
    cur->it = sets.begin();
  }

  /* Filter out any combined tests that do not have a kernel counterpart loaded */
  while (cur->it->second.wrapper.size() != 0 && cur->it != sets.end()) {
    if (cur->it->second.test_names.size() == 0)
      log(KTF_INFO, "Note: Skipping test suite %s which has combined tests with no kernel counterpart\n",
	  cur->it->first.c_str());
    ++(cur->it);
  }

  if (cur->it == sets.end()) {
    delete cur;
    cur = NULL;
    return stringvec();
  }

  stringvec& v = cur->it->second.test_names;
  cur->setname = cur->it->first;

  ++(cur->it);
  return v;
}


void *get_priv(KernelTest *kt, size_t sz)
{
  return kt->get_priv(sz);
}

size_t get_priv_sz(KernelTest *kt)
{
  return kt->user_priv_sz;
}

int set_coverage(std::string module, unsigned int opts, bool enabled)
{
  struct nl_msg *msg;
  int err;

  msg = nlmsg_alloc();
  genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0, NLM_F_REQUEST,
              KTF_C_REQ, 1);
  nla_put_u32(msg, KTF_A_TYPE,
  	      enabled ? KTF_CT_COV_ENABLE : KTF_CT_COV_DISABLE);
  nla_put_u32(msg, KTF_A_COVOPT, opts);
  nla_put_u64(msg, KTF_A_VERSION, KTF_VERSION_LATEST);
  nla_put_string(msg, KTF_A_MOD, module.c_str());

  // Send message over netlink socket
  nl_send_auto_complete(sock, msg);

  // Free message
  nlmsg_free(msg);

  //Wait for acknowledgement:
  // This function also returns error status if the message
  // was not deemed ok by the kernel.
  //
  err = nl_wait_for_ack(sock);
  if (err == 0) {
	// Then wait for the answer and receive it
	nl_recvmsgs_default(sock);
  }
  return err;
}

  KernelTest::KernelTest(const std::string& sn, const char* tn, unsigned int handle_id)
  : setname(sn),
    testname(tn),
    setnum(0),
    testnum(0),
    user_priv(NULL),
    user_priv_sz(0),
    user_test(NULL),
    file(NULL),
    line(-1)
{

  name = setname;
  name.append(".");
  name.append(testname);

  testset& ts(kmgr().find_add_test(setname, testname));
  setnum = ts.setnum;
  ts.tests[testname] = this;

  if (!handle_id)
    ts.test_names.push_back(testname);
  else {
    stringvec& ctxv = kmgr().get_contexts(handle_id);
    for (stringvec::iterator it = ctxv.begin(); it != ctxv.end(); ++it)
      ts.test_names.push_back(testname + "_" + *it);
  }
  testnum = ts.tests.size();

  wrappermap::iterator hit = ts.wrapper.find(testname);
  if (hit != ts.wrapper.end()) {
    log(KTF_DEBUG_V, "Assigning user_test from wrapper for %s.%s\n",
	setname.c_str(), testname.c_str());
    user_test = hit->second;
    /* Clear out wrapper entry as we skip any test sets
     * with nonempty wrapper lists during test execution:
     */
    ts.wrapper.erase(hit);
  }
}


KernelTest::~KernelTest()
{
  if (user_priv)
    free(user_priv);
}

void* KernelTest::get_priv(size_t p_sz)
{
  if (!user_priv) {
    user_priv = malloc(p_sz);
    if (user_priv)
      user_priv_sz = p_sz;
  }
  return user_priv;
}

static int parse_cb(struct nl_msg *msg, void *arg);
static int debug_cb(struct nl_msg *msg, void *arg);
static int error_cb(struct nl_msg *msg, void *arg);

int nl_connect(void)
{
  /* Allocate a new netlink socket */
  sock = nl_socket_alloc();
  if (sock == NULL){
    fprintf(stderr, "Failed to allocate a nl socket");
    exit(1);
  }

  /* Connect to generic netlink socket on kernel side */
  int stat = genl_connect(sock);
  if (stat) {
    fprintf(stderr, "Failed to open generic netlink connection");
    exit(1);
  }

  /* Ask kernel to resolve family name to family id */
  family = genl_ctrl_resolve(sock, "ktf");
  if (family <= 0) {
    fprintf(stderr, "Netlink protocol family for ktf not found - is the ktf module loaded?\n");
    exit(1);
  }

  /* Specify the generic callback functions for messages */
  nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, parse_cb, NULL);
  nl_socket_modify_cb(sock, NL_CB_INVALID, NL_CB_CUSTOM, error_cb, NULL);
  return 0;
}


void default_test_handler(int result,  const char* file, int line, const char* report)
{
  if (result >= 0) {
    fprintf(stderr, "default_test_handler: Result %d: %s,%d\n",result,file,line);
  } else {
    fprintf(stderr, "default_test_handler: Result %d\n",result);
  }
}

test_handler handle_test = default_test_handler;

bool setup(test_handler ht)
{
  ktf_debug_init();
  handle_test = ht;
  return nl_connect() == 0;
}


/* Query kernel for available tests in index order */
stringvec& query_testsets()
{
  struct nl_msg *msg;
  int err;

  msg = nlmsg_alloc();
  genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0, NLM_F_REQUEST,
	      KTF_C_REQ, 1);
  nla_put_u32(msg, KTF_A_TYPE, KTF_CT_QUERY);
  nla_put_u64(msg, KTF_A_VERSION, KTF_VERSION_LATEST);

  // Send message over netlink socket
  nl_send_auto_complete(sock, msg);

  // Free message
  nlmsg_free(msg);

  // Wait for acknowledgement:
  // This function also returns error status if the message
  // was not deemed ok by the kernel.
  //
  err = nl_wait_for_ack(sock);
  if (err < 0) {
    errno = -err;
    return kmgr().get_set_names();
  }

  // Then wait for the answer and receive it
  nl_recvmsgs_default(sock);
  return kmgr().get_set_names();
}

stringvec get_test_names()
{
  return kmgr().get_test_names();
}

std::string get_current_setname()
{
  return kmgr().get_current_setname();
}

KernelTest* find_test(const std::string&setname, const std::string& testname, std::string* ctx)
{
  return kmgr().find_test(setname, testname, ctx);
}

void add_wrapper(const std::string setname, const std::string testname, test_cb* tcb)
{
  kmgr().add_wrapper(setname, testname, tcb);
}

void run_test(KernelTest* kt, std::string& ctx)
{
  if (kt->user_test)
    kt->user_test->fun(kt);
  else
    run(kt, ctx);
}

/* Run the kernel test */
void run(KernelTest* kt, std::string context)
{
  struct nl_msg *msg;

  log(KTF_DEBUG_V, "START kernel test (%ld,%ld): %s\n", kt->setnum,
		kt->testnum, kt->name.c_str());

  msg = nlmsg_alloc();
  genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0, NLM_F_REQUEST,
	      KTF_C_REQ, 1);
  nla_put_u32(msg, KTF_A_TYPE, KTF_CT_RUN);
  nla_put_u64(msg, KTF_A_VERSION, KTF_VERSION_LATEST);
  nla_put_string(msg, KTF_A_SNAM, kt->setname.c_str());
  nla_put_string(msg, KTF_A_TNAM, kt->testname.c_str());

  if (!context.empty())
    nla_put_string(msg, KTF_A_STR, context.c_str());

  /* Send any test specific out-of-band data */
  if (kt->user_priv)
    nla_put(msg, KTF_A_DATA, kt->user_priv_sz, kt->user_priv);

  // Send message over netlink socket
  nl_send_auto_complete(sock, msg);

  // Free message
  nlmsg_free(msg);

  // Wait for acknowledgement - otherwise
  // nl_recvmsg_default will sometimes take the ack for the next message..
  int err = nl_wait_for_ack(sock);
  if (err < 0) {
    errno = -err;
    return;
  }

  // Wait for the answer and receive it
  nl_recvmsgs_default(sock);

  log(KTF_DEBUG_V, "END   ktf::run_kernel_test %s\n", kt->name.c_str());
}


void configure_context(const std::string context, unsigned type_id, void *data, size_t data_sz)
{
  context_vector ct = kmgr().find_contexts(context);
  ASSERT_GE(ct.size(), 1UL) << " - no context found named " << context;
  ASSERT_EQ(ct.size(), 1UL) << " - More than one context named " << context
			  << " - use KTF_CONTEXT_CFG_FOR_TEST to uniquely identify context.";
  ASSERT_EQ(type_id, ct[0]->Type());
  ASSERT_EQ(ct[0]->Configure(data, data_sz), 0);
}

void configure_context_for_test(const std::string& setname, const std::string& testname,
				unsigned type_id, void *data, size_t data_sz)
{
  std::string context;
  KernelTest *kt = kmgr().find_test(setname, testname, &context);
  context_vector ct = kmgr().find_contexts(context);
  ASSERT_TRUE(kt) << " Could not find test " << setname << "." << testname;
  int handle_id = kt->handle_id;
  ASSERT_NE(handle_id, 0) << " test " << setname << "." << testname << " does not have a context";

  for (context_vector::iterator it = ct.begin(); it != ct.end(); ++it)
    if ((*it)->handle_id == handle_id)
    {
      ASSERT_EQ(type_id, (*it)->Type());
      ASSERT_EQ((*it)->Configure(data, data_sz), 0);
      return;
    }
  ASSERT_TRUE(false) << " unconfigurable context found for test " << setname << "." << testname << "?";
}


static nl_cb_action parse_one_set(std::string& setname,
				  std::string& testname, struct nlattr* attr)
{
  int rem = 0;
  struct nlattr *nla;
  const char* msg;
  unsigned int handle_id = 0;

  nla_for_each_nested(nla, attr, rem) {
    switch (nla->nla_type) {
    case KTF_A_HID:
      handle_id = nla_get_u32(nla);
      break;
    case KTF_A_STR:
      msg = nla_get_string(nla);
      kmgr().add_test(setname, msg, handle_id);
      handle_id = 0;
      break;
    default:
      fprintf(stderr,"parse_result: Unexpected attribute type %d\n", nla->nla_type);
      return NL_SKIP;
    }
  }
  return NL_OK;
}



static int parse_query(struct nl_msg *msg, struct nlattr** attrs)
{
  int alloc = 0, rem = 0, rem2 = 0, cfg_stat;
  nl_cb_action stat;
  std::string setname,testname,ctx;

  /* Version 0.1.0.0 did not report version back from the kernel */
  uint64_t kernel_version = (KTF_VERSION_SET(MAJOR, 0ULL) | KTF_VERSION_SET(MINOR, 1ULL));

  if (attrs[KTF_A_VERSION])
    kernel_version = nla_get_u64(attrs[KTF_A_VERSION]);

  /* We only got here if we were compatible enough, log that we had differences */
  if (kernel_version != KTF_VERSION_LATEST)
  {
    const char* note = "Note";
    bool is_compatible =
      KTF_VERSION(MAJOR, KTF_VERSION_LATEST) == KTF_VERSION(MAJOR, kernel_version) &&
      KTF_VERSION(MINOR, KTF_VERSION_LATEST) == KTF_VERSION(MINOR, kernel_version);
    if (!is_compatible)
      note = "Error";

    fprintf(stderr,
	    "%s: KTF version difference - user lib %llu.%llu.%llu.%llu, kernel has %llu.%llu.%llu.%llu\n",
	    note,
	    KTF_VERSION(MAJOR, KTF_VERSION_LATEST),
	    KTF_VERSION(MINOR, KTF_VERSION_LATEST),
	    KTF_VERSION(MICRO, KTF_VERSION_LATEST),
	    KTF_VERSION(BUILD, KTF_VERSION_LATEST),
	    KTF_VERSION(MAJOR, kernel_version),
	    KTF_VERSION(MINOR, kernel_version),
	    KTF_VERSION(MICRO, kernel_version),
	    KTF_VERSION(BUILD, kernel_version));
    if (!is_compatible)
      return NL_SKIP;
  }

  if (attrs[KTF_A_HLIST]) {
    struct nlattr *nla, *nla2;
    stringvec contexts;
    unsigned int handle_id = 0;
    unsigned int type_id = 0;

    /* Parse info on handle IDs and associated contexts: */
    nla_for_each_nested(nla, attrs[KTF_A_HLIST], rem) {
      switch (nla->nla_type) {
      case KTF_A_HID:
	handle_id = nla_get_u32(nla);
	break;
      case KTF_A_LIST:
	nla_for_each_nested(nla2, nla, rem2) {
	  switch (nla2->nla_type) {
	  case KTF_A_STR:
	    ctx = nla_get_string(nla2);
	    contexts.push_back(ctx);
	    break;
	  case KTF_A_NUM:
	    type_id = nla_get_u32(nla2);
	    break;
	  case KTF_A_STAT:
	    cfg_stat = nla_get_u32(nla2);
	    kmgr().add_configurable_context(ctx, type_id, handle_id, cfg_stat);
	    break;
	  }
	}
	/* Add this set of contexts for the handle_id */
	kmgr().add_cset(handle_id, contexts);
	handle_id = 0;
	contexts.clear();
	break;
      default:
	fprintf(stderr,"parse_query: Unexpected attribute type %d\n", nla->nla_type);
	return NL_SKIP;
      }
    }
  }

  if (attrs[KTF_A_NUM]) {
    alloc = nla_get_u32(attrs[KTF_A_NUM]);
    log(KTF_DEBUG, "Kernel offers %d test sets:\n", alloc);
  } else {
    fprintf(stderr,"No test set count in kernel response??\n");
    return -1;
  }

  if (attrs[KTF_A_LIST]) {
    struct nlattr *nla;

    /* Parse info on test sets */
    nla_for_each_nested(nla, attrs[KTF_A_LIST], rem) {
      switch (nla->nla_type) {
      case KTF_A_STR:
	setname = nla_get_string(nla);
	break;
      case KTF_A_TEST:
	stat = parse_one_set(setname, testname, nla);
	if (stat != NL_OK)
	  return stat;
	break;
      default:
	fprintf(stderr,"parse_result: Unexpected attribute type %d\n", nla->nla_type);
	return NL_SKIP;
      }
      kmgr().find_add_set(setname); /* Just to make sure empty sets are also added */
    }
  }

  return NL_OK;
}


static enum nl_cb_action parse_result(struct nl_msg *msg, struct nlattr** attrs)
{
  int assert_cnt = 0, fail_cnt = 0;
  int rem = 0, stat;
  const char *file = "no_file",*report = "no_report";

  if (attrs[KTF_A_STAT]) {
    stat = nla_get_u32(attrs[KTF_A_STAT]);
    log(KTF_DEBUG, "parsed test status %d\n", stat);
    if (stat) {
      fprintf(stderr, "Failed to execute test in kernel - status %d\n", stat);
    }
  }
  if (attrs[KTF_A_LIST]) {
    /* Parse list of test results */
    struct nlattr *nla;
    int result = -1, line = 0;
    nla_for_each_nested(nla, attrs[KTF_A_LIST], rem) {
      switch (nla->nla_type) {
      case KTF_A_STAT:
	/* Flush previous test, if any */
	handle_test(result,file,line,report);
	result = nla_get_u32(nla);
	/* Our own count and report since check does such a lousy
	 * job in counting individual checks */
	if (result)
	  assert_cnt += result;
	else {
	  fail_cnt++;
	  assert_cnt++;
	}
	break;
      case KTF_A_FILE:
	file = nla_get_string(nla);
	if (!file)
	  file = "no_file";
	break;
      case KTF_A_NUM:
	line = nla_get_u32(nla);
	break;
      case KTF_A_STR:
	report = nla_get_string(nla);
	if (!report)
	  report = "no_report";
	break;
      default:
	fprintf(stderr,"parse_result: Unexpected attribute type %d\n", nla->nla_type);
	return NL_SKIP;
      }
    }
    /* Handle last test */
    handle_test(result,file,line,report);
  }

  return NL_OK;
}


static int parse_cb(struct nl_msg *msg, void *arg)
{
  struct nlmsghdr *nlh = nlmsg_hdr(msg);
  int maxtype = KTF_A_MAX+10;
  struct nlattr *attrs[maxtype];
  enum ktf_cmd_type type;

  //  memset(attrs, 0, sizeof(attrs));

  /* Validate message and parse attributes */
  int err = genlmsg_parse(nlh, 0, attrs, KTF_A_MAX, get_ktf_gnl_policy());
  if (err < 0) return err;

  if (!attrs[KTF_A_TYPE]) {
    fprintf(stderr, "Received kernel response without a type\n");
    return NL_SKIP;
  }

  type = (ktf_cmd_type)nla_get_u32(attrs[KTF_A_TYPE]);
  switch (type) {
  case KTF_CT_QUERY:
    return parse_query(msg, attrs);
  case KTF_CT_RUN:
    return parse_result(msg, attrs);
  default:
    debug_cb(msg, attrs);
  }
  return NL_SKIP;
}


static int error_cb(struct nl_msg *msg, void *arg)
{
  struct nlmsghdr *nlh = nlmsg_hdr(msg);
  fprintf(stderr, "Received invalid netlink message - type %d\n", nlh->nlmsg_type);
  return NL_OK;
}


static int debug_cb(struct nl_msg *msg, void *arg)
{
  struct nlmsghdr *nlh = nlmsg_hdr(msg);
  fprintf(stderr, "[Received netlink message of type %d]\n", nlh->nlmsg_type);
    nl_msg_dump(msg, stderr);
    return NL_OK;
}

} // end namespace ktf
