/**
 * (C) Copyright 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * This file is part of DAOS
 * src/tests/suite/daos_test.h
 */
#ifndef __DAOS_TEST_H
#define __DAOS_TEST_H

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <time.h>
#include <cmocka.h>
#include <mpi.h>
#include <daos/common.h>
#include <daos/mgmt.h>
#include <daos/tests_lib.h>
#include <daos.h>

/** Server crt group ID */
extern const char *server_group;

/** Pool service replicas */
extern unsigned int svc_nreplicas;

extern int daos_event_priv_reset(void);
#define TEST_RANKS_MAX_NUM	(13)

/* the pool used for daos test suite */
struct test_pool {
	d_rank_t		ranks[TEST_RANKS_MAX_NUM];
	uuid_t			pool_uuid;
	daos_handle_t		poh;
	daos_pool_info_t	pool_info;
	daos_size_t		pool_size;
	d_rank_list_t		svc;
	/* flag of slave that share the pool of other test_arg_t */
	bool			slave;
	bool			destroyed;
};

typedef struct {
	bool			multi_rank;
	int			myrank;
	int			rank_size;
	const char	       *group;
	struct test_pool	pool;
	uuid_t			co_uuid;
	unsigned int		mode;
	unsigned int		uid;
	unsigned int		gid;
	daos_handle_t		eq;
	daos_handle_t		coh;
	daos_cont_info_t	co_info;
	int			setup_state;
	bool			async;
	bool			hdl_share;
	uint64_t		fail_loc;
	uint64_t		fail_value;
	bool			overlap;
	int			expect_result;
	daos_size_t		size;
	int			nr;
	int			srv_ntgts;
	int			srv_disabled_ntgts;
	int			index;

	/* The callback is called before pool rebuild. like disconnect
	 * pool etc.
	 */
	int			(*rebuild_pre_cb)(void *test_arg);
	void			*rebuild_pre_cb_arg;

	/* The callback is called during pool rebuild, used for concurrent IO,
	 * container destroy etc
	 */
	int			(*rebuild_cb)(void *test_arg);
	void			*rebuild_cb_arg;
	/* The callback is called after pool rebuild, used for validating IO
	 * after rebuild
	 */
	int			(*rebuild_post_cb)(void *test_arg);
	void			*rebuild_post_cb_arg;
} test_arg_t;

enum {
	SETUP_EQ,
	SETUP_POOL_CREATE,
	SETUP_POOL_CONNECT,
	SETUP_CONT_CREATE,
	SETUP_CONT_CONNECT,
};

#define DEFAULT_POOL_SIZE	(4ULL << 30)

#define WAIT_ON_ASYNC_ERR(arg, ev, err)			\
	do {						\
		int rc;					\
		daos_event_t *evp;			\
							\
		if (!arg->async)			\
			break;				\
							\
		rc = daos_eq_poll(arg->eq, 1,		\
				  DAOS_EQ_WAIT,		\
				  1, &evp);		\
		assert_int_equal(rc, 1);		\
		assert_ptr_equal(evp, &ev);		\
		assert_int_equal(ev.ev_error, err);	\
	} while (0)

#define WAIT_ON_ASYNC(arg, ev) WAIT_ON_ASYNC_ERR(arg, ev, 0)

int
test_teardown(void **state);
int
test_setup(void **state, unsigned int step, bool multi_rank,
	   daos_size_t pool_size, struct test_pool *pool);

static inline int
async_enable(void **state)
{
	test_arg_t	*arg = *state;

	arg->overlap = false;
	arg->async   = true;
	return 0;
}

static inline int
async_disable(void **state)
{
	test_arg_t	*arg = *state;

	arg->overlap = false;
	arg->async   = false;
	return 0;
}

static inline int
async_overlap(void **state)
{
	test_arg_t	*arg = *state;

	arg->overlap = true;
	arg->async   = true;
	return 0;
}

static inline int
test_case_teardown(void **state)
{
	assert_int_equal(daos_event_priv_reset(), 0);
	return 0;
}

static inline int
hdl_share_enable(void **state)
{
	test_arg_t	*arg = *state;

	arg->hdl_share = true;
	return 0;
}

enum {
	HANDLE_POOL,
	HANDLE_CO
};

int run_daos_mgmt_test(int rank, int size);
int run_daos_pool_test(int rank, int size);
int run_daos_cont_test(int rank, int size);
int run_daos_capa_test(int rank, int size);
int run_daos_io_test(int rank, int size, int *tests, int test_size);
int run_daos_array_test(int rank, int size);
int run_daos_epoch_test(int rank, int size);
int run_daos_epoch_recovery_test(int rank, int size);
int run_daos_md_replication_test(int rank, int size);
int run_daos_oid_alloc_test(int rank, int size);
int run_daos_degraded_test(int rank, int size);
int run_daos_rebuild_test(int rank, int size, int *tests, int test_size);

void daos_kill_server(test_arg_t *arg, const uuid_t pool_uuid, const char *grp,
		      d_rank_list_t *svc, d_rank_t rank);
void daos_exclude_server(const uuid_t pool_uuid, const char *grp,
			 const d_rank_list_t *svc, d_rank_t rank);
void daos_kill_exclude_server(test_arg_t *arg, const uuid_t pool_uuid,
			      const char *grp, d_rank_list_t *svc);
void daos_add_server(const uuid_t pool_uuid, const char *grp,
		     const d_rank_list_t *svc, d_rank_t rank);
typedef int (*test_setup_cb_t)(void **state);

int run_daos_sub_tests(const struct CMUnitTest *tests, int tests_size,
		       daos_size_t pool_size, int *sub_tests,
		       int sub_tests_size, test_setup_cb_t cb);

static inline void
daos_test_print(int rank, char *message)
{
	if (!rank)
		print_message("%s\n", message);
}

static inline void
handle_share(daos_handle_t *hdl, int type, int rank, daos_handle_t poh,
	     int verbose)
{
	daos_iov_t	ghdl = { NULL, 0, 0 };
	int		rc;

	if (rank == 0) {
		/** fetch size of global handle */
		if (type == HANDLE_POOL)
			rc = daos_pool_local2global(*hdl, &ghdl);
		else
			rc = daos_cont_local2global(*hdl, &ghdl);
		assert_int_equal(rc, 0);
	}

	/** broadcast size of global handle to all peers */
	rc = MPI_Bcast(&ghdl.iov_buf_len, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
	assert_int_equal(rc, MPI_SUCCESS);

	/** allocate buffer for global pool handle */
	ghdl.iov_buf = malloc(ghdl.iov_buf_len);
	ghdl.iov_len = ghdl.iov_buf_len;

	if (rank == 0) {
		/** generate actual global handle to share with peer tasks */
		if (verbose)
			print_message("rank 0 call local2global on %s handle",
				      (type == HANDLE_POOL) ?
				      "pool" : "container");
		if (type == HANDLE_POOL)
			rc = daos_pool_local2global(*hdl, &ghdl);
		else
			rc = daos_cont_local2global(*hdl, &ghdl);
		assert_int_equal(rc, 0);
		if (verbose)
			print_message("success\n");
	}

	/** broadcast global handle to all peers */
	if (rank == 0 && verbose == 1)
		print_message("rank 0 broadcast global %s handle ...",
			      (type == HANDLE_POOL) ? "pool" : "container");
	rc = MPI_Bcast(ghdl.iov_buf, ghdl.iov_len, MPI_BYTE, 0,
		       MPI_COMM_WORLD);
	assert_int_equal(rc, MPI_SUCCESS);
	if (rank == 0 && verbose == 1)
		print_message("success\n");

	if (rank != 0) {
		/** unpack global handle */
		if (verbose)
			print_message("rank %d call global2local on %s handle",
				      rank, type == HANDLE_POOL ?
				      "pool" : "container");
		if (type == HANDLE_POOL) {
			/* NB: Only pool_global2local are different */
			rc = daos_pool_global2local(ghdl, hdl);
		} else {
			rc = daos_cont_global2local(poh, ghdl, hdl);
		}

		assert_int_equal(rc, 0);
		if (verbose)
			print_message("rank %d global2local success\n", rank);
	}

	free(ghdl.iov_buf);

	MPI_Barrier(MPI_COMM_WORLD);
}

#define MAX_KILLS	3
extern d_rank_t ranks_to_kill[MAX_KILLS];
bool test_runable(test_arg_t *arg, unsigned int required_tgts);
int test_pool_get_info(test_arg_t *arg, daos_pool_info_t *pinfo);
int test_get_leader(test_arg_t *arg, d_rank_t *rank);
bool test_rebuild_query(test_arg_t **args, int args_cnt);
void test_rebuild_wait(test_arg_t **args, int args_cnt);

#endif
