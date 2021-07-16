/**
 * (C) Copyright 2018-2021 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#define D_LOGFAC       DD_FAC(tests)

#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <abt.h>
#include <daos/common.h>
#include <daos/tests_lib.h>
#include <daos_srv/vos.h>
#include <daos_test.h>
#include <daos/dts.h>
#include "perf_internal.h"

uint64_t	ts_flags;

char		ts_pmem_file[PATH_MAX];
bool		ts_zero_copy;	/* use zero-copy API for VOS */
bool		ts_nest_iterator;

daos_unit_oid_t	*ts_uoids;	/* object shard IDs */

bool		ts_in_ult;	/* Run tests in ULT mode */
static ABT_xstream	abt_xstream;

static int
ts_abt_init(void)
{
	int cpuid;
	int num_cpus;
	int rc;

	rc = ABT_init(0, NULL);
	if (rc != ABT_SUCCESS) {
		fprintf(stderr, "ABT init failed: %d\n", rc);
		return -1;
	}

	rc = ABT_xstream_self(&abt_xstream);
	if (rc != ABT_SUCCESS) {
		printf("ABT get self xstream failed: %d\n", rc);
		return -1;
	}

	rc = ABT_xstream_get_cpubind(abt_xstream, &cpuid);
	if (rc != ABT_SUCCESS) {
		fprintf(stderr, "get cpubind failed: %d\n", rc);
		fprintf(stderr, "No CPU affinity for this test.\n");
		fprintf(stderr, "Build ABT by --enable-affinity if"
			" you want to try CPU affinity.\n");
		return 0;
	}

	rc = ABT_xstream_get_affinity(abt_xstream, 0, NULL, &num_cpus);
	if (rc != ABT_SUCCESS) {
		fprintf(stderr, "get num_cpus: %d\n", rc);
		fprintf(stderr, "No CPU affinity for this test.\n");
		fprintf(stderr, "Build ABT by --enable-affinity if"
			" you want to try CPU affinity.\n");
		return 0;
	}

	cpuid = (cpuid + 1) % num_cpus;
	rc = ABT_xstream_set_cpubind(abt_xstream, cpuid);
	if (rc != ABT_SUCCESS) {
		fprintf(stderr, "set affinity: %d\n", rc);
		fprintf(stderr, "No CPU affinity for this test.\n");
		fprintf(stderr, "Build ABT by --enable-affinity if"
			" you want to try CPU affinity.\n");
		return 0;
	}

	return 0;
}

static void
ts_abt_fini(void)
{
	ABT_xstream_join(abt_xstream);
	ABT_xstream_free(&abt_xstream);
	ABT_finalize();
}

static int
_vos_update_or_fetch(int obj_idx, enum ts_op_type op_type,
		     struct io_credit *cred, daos_epoch_t epoch,
		     double *duration)
{
	uint64_t	start = 0;
	int		rc = 0;

	TS_TIME_START(duration, start);
	if (!ts_zero_copy) {
		if (op_type == TS_DO_UPDATE)
			rc = vos_obj_update(ts_ctx.tsc_coh, ts_uoids[obj_idx],
					    epoch, 0, 0, &cred->tc_dkey, 1,
					    &cred->tc_iod, NULL, &cred->tc_sgl);
		else
			rc = vos_obj_fetch(ts_ctx.tsc_coh, ts_uoids[obj_idx],
					   epoch, 0, &cred->tc_dkey, 1,
					   &cred->tc_iod, &cred->tc_sgl);
	} else { /* zero-copy */
		struct bio_sglist	*bsgl;
		daos_handle_t		 ioh;

		if (op_type == TS_DO_UPDATE)
			rc = vos_update_begin(ts_ctx.tsc_coh, ts_uoids[obj_idx],
					      epoch, 0, &cred->tc_dkey, 1,
					      &cred->tc_iod, NULL, 0, &ioh,
					      NULL);
		else
			rc = vos_fetch_begin(ts_ctx.tsc_coh, ts_uoids[obj_idx],
					     epoch, &cred->tc_dkey, 1,
					     &cred->tc_iod, 0, NULL, &ioh,
					     NULL);
		if (rc)
			return rc;

		rc = bio_iod_prep(vos_ioh2desc(ioh), BIO_CHK_TYPE_IO, NULL, 0);
		if (rc)
			goto end;

		bsgl = vos_iod_sgl_at(ioh, 0);
		D_ASSERT(bsgl != NULL);
		D_ASSERT(bsgl->bs_nr_out == 1);
		D_ASSERT(cred->tc_sgl.sg_nr == 1);

		if (op_type == TS_DO_FETCH) {
			memcpy(cred->tc_sgl.sg_iovs[0].iov_buf,
			       bio_iov2raw_buf(&bsgl->bs_iovs[0]),
			       bio_iov2raw_len(&bsgl->bs_iovs[0]));
		} else {
			memcpy(bio_iov2req_buf(&bsgl->bs_iovs[0]),
			       cred->tc_sgl.sg_iovs[0].iov_buf,
			       cred->tc_sgl.sg_iovs[0].iov_len);
		}

		rc = bio_iod_post(vos_ioh2desc(ioh));
end:
		if (op_type == TS_DO_UPDATE)
			rc = vos_update_end(ioh, 0, &cred->tc_dkey, rc, NULL,
					    NULL);
		else
			rc = vos_fetch_end(ioh, NULL, rc);
	}

	TS_TIME_END(duration, start);
	return rc;
}

struct vos_ult_arg {
	struct io_credit	*cred;
	double			*duration;
	daos_epoch_t		 epoch;
	enum ts_op_type		 op_type;
	int			 obj_idx;
	int			 status;
};

static void
vos_update_or_fetch_ult(void *arg)
{
	struct vos_ult_arg *ult_arg = arg;

	ult_arg->status = _vos_update_or_fetch(ult_arg->obj_idx,
					       ult_arg->op_type,
					       ult_arg->cred,
					       ult_arg->epoch,
					       ult_arg->duration);
}

static int
vos_update_or_fetch(int obj_idx, enum ts_op_type op_type,
		    struct io_credit *cred, daos_epoch_t epoch,
		    bool sync, double *duration)
{
	ABT_thread		thread;
	struct vos_ult_arg	ult_arg;
	int			rc;

	if (!ts_in_ult)
		return _vos_update_or_fetch(obj_idx, op_type, cred, epoch,
					    duration);

	ult_arg.op_type = op_type;
	ult_arg.cred = cred;
	ult_arg.epoch = epoch;
	ult_arg.duration = duration;
	ult_arg.obj_idx = obj_idx;
	rc = ABT_thread_create_on_xstream(abt_xstream, vos_update_or_fetch_ult,
					  &ult_arg, ABT_THREAD_ATTR_NULL,
					  &thread);
	if (rc != ABT_SUCCESS)
		return rc;

	rc = ABT_thread_join(thread);
	if (rc != ABT_SUCCESS)
		return rc;

	ABT_thread_free(&thread);
	rc = ult_arg.status;
	return rc;
}

static int
objects_query(struct pf_param *param)
{
	daos_epoch_t epoch = crt_hlc_get();
	char		*akey = "0";
	d_iov_t		dkey_iov;
	d_iov_t		akey_iov;
	daos_recx_t	recx;
	int		i;
	int		rc = 0;
	uint64_t	start = 0;


	d_iov_set(&akey_iov, akey, 1);

	TS_TIME_START(&param->pa_duration, start);

	for (i = 0; i < ts_obj_p_cont; i++) {
		d_iov_set(&dkey_iov, NULL, 0);
		rc = vos_obj_query_key(ts_ctx.tsc_coh, ts_uoids[i],
				       DAOS_GET_MAX | DAOS_GET_DKEY |
				       DAOS_GET_RECX, epoch, &dkey_iov,
				       &akey_iov, &recx, NULL);
		if (rc != 0 && rc != -DER_NONEXIST)
			break;
		if (param->pa_query.verbose) {
			if (rc == -DER_NONEXIST) {
				printf("query_key "DF_UOID ": -DER_NONEXIST\n",
				       DP_UOID(ts_uoids[i]));
			} else {
				printf("query_key "DF_UOID ": dkey="DF_U64
				       " recx="DF_RECX"\n",
				       DP_UOID(ts_uoids[i]),
				       *(uint64_t *)dkey_iov.iov_buf,
				       DP_RECX(recx));
			}
		}
		rc = 0;
	}

	TS_TIME_END(&param->pa_duration, start);

	return rc;
}

typedef int (*iterate_cb_t)(daos_handle_t ih, vos_iter_entry_t *key_ent,
			    vos_iter_param_t *param);

static int
ts_iterate_internal(uint32_t type, vos_iter_param_t *param,
		    iterate_cb_t iter_cb)
{
	daos_anchor_t		*probe_hash = NULL;
	vos_iter_entry_t	key_ent;
	daos_handle_t		ih;
	int			rc;

	rc = vos_iter_prepare(type, param, &ih, NULL);
	if (rc != 0) {
		if (rc == -DER_NONEXIST)
			rc = 0;
		else
			D_ERROR("Failed to prepare d-key iterator: "DF_RC"\n",
				DP_RC(rc));
		D_GOTO(out, rc);
	}

	rc = vos_iter_probe(ih, probe_hash);
	if (rc != 0) {
		if (rc == -DER_NONEXIST || rc == -DER_AGAIN)
			rc = 0;
		D_GOTO(out_iter_fini, rc);
	}

	while (1) {
		rc = vos_iter_fetch(ih, &key_ent, NULL);
		if (rc != 0)
			break;

		/* fill the key to iov if there are enough space */
		if (iter_cb) {
			rc = iter_cb(ih, &key_ent, param);
			if (rc != 0)
				break;
		}

		rc = vos_iter_next(ih);
		if (rc)
			break;
	}

	if (rc == -DER_NONEXIST)
		rc = 0;

out_iter_fini:
	vos_iter_finish(ih);
out:
	return rc;
}

static int
iter_akey_cb(daos_handle_t ih, vos_iter_entry_t *key_ent,
	     vos_iter_param_t *param)
{
	int	rc;

	param->ip_akey = key_ent->ie_key;
	/* iterate array record */
	if (ts_nest_iterator)
		param->ip_ih = ih;

	rc = ts_iterate_internal(VOS_ITER_RECX, param, NULL);
	if (rc)
		return rc;

	rc = ts_iterate_internal(VOS_ITER_SINGLE, param, NULL);
	return rc;
}

static int
iter_dkey_cb(daos_handle_t ih, vos_iter_entry_t *key_ent,
	     vos_iter_param_t *param)
{
	int	rc;

	param->ip_dkey = key_ent->ie_key;
	if (ts_nest_iterator)
		param->ip_ih = ih;
	/* iterate akey */
	rc = ts_iterate_internal(VOS_ITER_AKEY, param, iter_akey_cb);
	return rc;
}

/* Iterate all of dkey/akey/record */
static int
obj_iter_records(daos_unit_oid_t oid, struct pf_param *ppa)
{
	vos_iter_param_t	param = {};
	int			rc = 0;
	uint64_t		start = 0;

	/* prepare iterate parameters */
	param.ip_hdl = ts_ctx.tsc_coh;
	param.ip_oid = oid;

	param.ip_epr.epr_lo = 0;
	param.ip_epr.epr_hi = DAOS_EPOCH_MAX;
	param.ip_epc_expr = VOS_IT_EPC_RE;

	TS_TIME_START(&ppa->pa_duration, start);
	rc = ts_iterate_internal(VOS_ITER_DKEY, &param, iter_dkey_cb);
	TS_TIME_END(&ppa->pa_duration, start);
	return rc;
}

static int
objects_open(void)
{
	int	i;

	for (i = 0; i < ts_obj_p_cont; i++) {
		if (!ts_oid_init) {
			ts_oids[i] = daos_test_oid_gen(
				DAOS_HDL_INVAL, DAOS_OC_RAW, ts_flags, 0,
				ts_ctx.tsc_mpi_rank);
		}

		ts_uoids[i].id_pub = ts_oids[i];
		ts_uoids[i].id_shard = 0;
		ts_uoids[i].id_pad_32 = 0;
	}
	ts_oid_init = true;
	return 0;
}

static int
objects_close(void)
{
	return 0;
}

static int
pf_update(struct pf_test *ts, struct pf_param *param)
{
	int	rc;

	rc = objects_open();
	if (rc)
		return rc;

	rc = objects_update(param);
	if (rc)
		return rc;

	rc = objects_close();
	return rc;
}

static int
pf_fetch(struct pf_test *ts, struct pf_param *param)
{
	int	rc;

	rc = objects_open();
	if (rc)
		return rc;

	param->pa_rw.verify = false;
	rc = objects_fetch(param);
	if (rc)
		return rc;

	rc = objects_close();
	return rc;
}

static int
pf_verify(struct pf_test *ts, struct pf_param *param)
{
	int	rc;

	if (ts_single && ts_recx_p_akey > 1) {
		fprintf(stdout, "Verification is unsupported\n");
		return 0;
	}

	rc = objects_open();
	if (rc)
		return rc;

	param->pa_rw.verify = true;
	rc = objects_fetch(param);
	if (rc)
		return rc;

	rc = objects_close();
	return rc;
}


static int
pf_iterate(struct pf_test *pf, struct pf_param *param)
{
	ts_nest_iterator = param->pa_iter.nested;
	return obj_iter_records(ts_uoids[0], param);
}

static int
pf_query(struct pf_test *ts, struct pf_param *param)
{
	int rc;

	if ((ts_flags & DAOS_OF_DKEY_UINT64) == 0) {
		fprintf(stderr, "Integer dkeys required for query test (-i)\n");
		return -1;
	}

	if (ts_single) {
		fprintf(stderr, "Array values required for query test (-A)\n");
		return -1;
	}

	if (!ts_const_akey) {
		fprintf(stderr, "Const akey required for query test (-I)\n");
		return -1;
	}

	rc = objects_open();
	if (rc)
		return rc;

	rc = objects_query(param);
	if (rc)
		return rc;

	rc = objects_close();
	return rc;
}

static int
pf_parse_query_cb(char *str, struct pf_param *pa, char **strp)
{
	switch (*str) {
	default:
		str++;
		break;
	case 'v':
		pa->pa_query.verbose = true;
		str++;
		break;
	}
	*strp = str;
	return 0;
}

/**
 * Example: "U;p Q;p;"
 * 'U' is update test.  Integer dkey required
 *	'p': parameter of update and it means outputting performance result
 *
 * 'Q' is query test
 *	'p': parameter of query and it means outputting performance result
 *	'v' enables verbosity
 */
static int
pf_parse_query(char *str, struct pf_param *pa, char **strp)
{
	return pf_parse_common(str, pa, pf_parse_query_cb, strp);
}

static int
pf_parse_iterate_cb(char *str, struct pf_param *pa, char **strp)
{
	switch (*str) {
	default:
		str++;
		break;
	case 'n':
		pa->pa_iter.nested = true;
		str++;
		break;
	}
	*strp = str;
	return 0;
}

static int
pf_parse_iterate(char *str, struct pf_param *pa, char **strp)
{
	return pf_parse_common(str, pa, pf_parse_iterate_cb, strp);
}

/* predefined test cases */
struct pf_test pf_tests[] = {
	{
		.ts_code	= 'U',
		.ts_name	= "UPDATE",
		.ts_parse	= pf_parse_rw,
		.ts_func	= pf_update,
	},
	{
		.ts_code	= 'F',
		.ts_name	= "FETCH",
		.ts_parse	= pf_parse_rw,
		.ts_func	= pf_fetch,
	},
	{
		.ts_code	= 'V',
		.ts_name	= "VERIFY",
		.ts_parse	= pf_parse_rw,
		.ts_func	= pf_verify,
	},
	{
		.ts_code	= 'I',
		.ts_name	= "ITERATE",
		.ts_parse	= pf_parse_iterate,
		.ts_func	= pf_iterate,
	},
	{
		.ts_code	= 'Q',
		.ts_name	= "QUERY",
		.ts_parse	= pf_parse_query,
		.ts_func	= pf_query,
	},
	{
		.ts_code	= 0,
	},
};

static inline const char *
ts_yes_or_no(bool value)
{
	return value ? "yes" : "no";
}

const char perf_vos_usage[] = "\n"
"-f pathname\n"
"	Full path name of the VOS file.\n\n"
"-z	Use zero copy API.\n\n"
"-i	Use integer dkeys.  Required if running QUERY test.\n\n"
"-I	Use constant akey.  Required for QUERY test.\n\n"
"-x	Run each test in an ABT ULT.\n";

static void
ts_print_usage(void)
{
	printf("vos_perf -- performance benchmark tool for VOS\n\n\
Description:\n\
	The vos_perf utility benchmarks point-to-point I/O performance of\n\
	different layers of the VOS stack.\n");
	printf("%s", perf_common_usage);
	printf("%s", perf_vos_usage);
}

const struct option perf_vos_opts[] = {
	{ "file",	required_argument,	NULL,	'f' },
	{ "zcopy",	no_argument,		NULL,	'z' },
	{ "int_dkey",	no_argument,		NULL,	'i' },
	{ "const_akey",	no_argument,		NULL,	'I' },
	{ "abt_ult",	no_argument,		NULL,	'x' },
	{ NULL,		0,			NULL,	0   },
};

const char perf_vos_optstr[] = "f:ziIx";

extern struct io_engine vos_engine;

int
main(int argc, char **argv)
{
	char		*cmds = NULL;
	char		uuid_buf[256];
	int		credits = -1;	/* sync mode */
	struct option	*ts_opts;
	char		*ts_optstr;
	int		rc, ret;

	ts_dkey_prefix = PF_DKEY_PREF;
	ts_flags = 0;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &ts_ctx.tsc_mpi_rank);
	MPI_Comm_size(MPI_COMM_WORLD, &ts_ctx.tsc_mpi_size);

	rc = perf_alloc_opts(perf_vos_opts,
			     sizeof(perf_vos_opts)/sizeof(perf_vos_opts[0]),
			     perf_vos_optstr, &ts_opts, &ts_optstr);
	if (rc)
		return rc;

	memset(ts_pmem_file, 0, sizeof(ts_pmem_file));
	while ((rc = getopt_long(argc, argv, ts_optstr, ts_opts, NULL)) != -1) {
		switch (rc) {
		default:
			ret = perf_parse_opts(rc, &cmds);
			if (ret) {
				perf_free_opts(ts_opts, ts_optstr);
				if (ret == 1) {
					ts_print_usage();
					return 0;
				}
				return ret;
			}
			break;
		case 'f':
			if (strnlen(optarg, PATH_MAX) >= (PATH_MAX - 5)) {
				fprintf(stderr, "filename size must be < %d\n",
					PATH_MAX - 5);
				perf_free_opts(ts_opts, ts_optstr);
				return -1;
			}
			strncpy(ts_pmem_file, optarg, PATH_MAX - 5);
			break;
		case 'z':
			ts_zero_copy = true;
			break;
		case 'i':
			ts_flags |= DAOS_OF_DKEY_UINT64;
			ts_dkey_prefix = NULL;
			break;
		case 'I':
			ts_const_akey = true;
			break;
		case 'x':
			ts_in_ult = true;
			break;
		}
	}
	perf_free_opts(ts_opts, ts_optstr);

	if (ts_const_akey)
		ts_akey_p_dkey = 1;

	if (!cmds) {
		D_PRINT("Please provide command string\n");
		ts_print_usage();
		return -1;
	}

	if (ts_seed == 0) {
		struct timeval	tv;

		gettimeofday(&tv, NULL);
		ts_seed = tv.tv_usec;
	}

	if (ts_dkey_p_obj == 0 || ts_akey_p_dkey == 0 || ts_recx_p_akey == 0) {
		fprintf(stderr, "Invalid arguments %d/%d/%d/\n",
			ts_dkey_p_obj, ts_akey_p_dkey, ts_recx_p_akey);
		if (ts_ctx.tsc_mpi_rank == 0)
			ts_print_usage();
		return -1;
	}

	if (ts_ctx.tsc_mpi_size > 1 &&
	    (access("/etc/daos_nvme.conf", F_OK) != -1)) {
		fprintf(stderr, "no support: multi-proc vos_perf with NVMe\n");
			return -1;
	}

	ts_ctx.tsc_cred_nr = -1; /* VOS can only support sync mode */
	if (strlen(ts_pmem_file) == 0) {
		snprintf(ts_pmem_file, sizeof(ts_pmem_file),
			 "/mnt/daos/vos_perf%d.pmem", ts_ctx.tsc_mpi_rank);
	} else {
		char id[16];

		snprintf(id, sizeof(id), "%d", ts_ctx.tsc_mpi_rank);
		strncat(ts_pmem_file, id,
			(sizeof(ts_pmem_file) - strlen(ts_pmem_file)));
	}
	ts_ctx.tsc_pmem_file = ts_pmem_file;

	if (ts_in_ult) {
		rc = ts_abt_init();
		if (rc)
			return rc;
	}

	if (ts_stride < STRIDE_MIN)
		ts_stride = STRIDE_MIN;

	stride_buf_init(ts_stride);

	ts_ctx.tsc_cred_vsize	= ts_stride;
	ts_ctx.tsc_scm_size	= ts_scm_size;
	ts_ctx.tsc_nvme_size	= ts_nvme_size;

	/*
	 * For vos_perf, if pool/cont uuids are supplied as command line
	 * arguments it's assumed that the pool/cont were created. If only a
	 * cont uuid is supplied then a pool and container will be created and
	 * the cont uuid will be used during creation
	 */
	if (!uuid_is_null(ts_ctx.tsc_pool_uuid)) {
		ts_ctx.tsc_skip_pool_create = true;
		if (!uuid_is_null(ts_ctx.tsc_cont_uuid))
			ts_ctx.tsc_skip_cont_create = true;
	}

	if (!ts_ctx.tsc_skip_cont_create)
		uuid_generate(ts_ctx.tsc_cont_uuid);
	if (!ts_ctx.tsc_skip_pool_create)
		uuid_generate(ts_ctx.tsc_pool_uuid);

	ts_update_or_fetch_fn = vos_update_or_fetch;

	rc = dts_ctx_init(&ts_ctx, &vos_engine);
	if (rc)
		return -1;

	memset(uuid_buf, 0, sizeof(uuid_buf));
	uuid_unparse(ts_ctx.tsc_pool_uuid, uuid_buf);

	if (ts_ctx.tsc_mpi_rank == 0) {
		fprintf(stdout,
			"Test :\n\t%s\n"
			"Pool :\n\t%s\n"
			"Parameters :\n"
			"\tpool size     : SCM: %u MB, NVMe: %u MB\n"
			"\tcredits       : %d (sync I/O for -ve)\n"
			"\tobj_per_cont  : %u x %d (procs)\n"
			"\tdkey_per_obj  : %u (%s)\n"
			"\takey_per_dkey : %u%s\n"
			"\trecx_per_akey : %u\n"
			"\tvalue type    : %s\n"
			"\tstride size   : %u\n"
			"\tzero copy     : %s\n"
			"\tVOS file      : %s\n",
			pf_class2name(DAOS_OC_RAW), uuid_buf,
			(unsigned int)(ts_scm_size >> 20),
			(unsigned int)(ts_nvme_size >> 20),
			credits,
			ts_obj_p_cont,
			ts_ctx.tsc_mpi_size,
			ts_dkey_p_obj, ts_dkey_prefix == NULL ? "int" : "buf",
			ts_akey_p_dkey, ts_const_akey ? " (const)" : "",
			ts_recx_p_akey,
			ts_val_type(),
			ts_stride,
			ts_yes_or_no(ts_zero_copy),
			ts_pmem_file);
	}

	ts_ohs = calloc(ts_obj_p_cont, sizeof(*ts_ohs));
	ts_oids = calloc(ts_obj_p_cont, sizeof(*ts_oids));
	ts_uoids = calloc(ts_obj_p_cont, sizeof(*ts_uoids));
	if (!ts_ohs || !ts_oids || !ts_uoids) {
		fprintf(stderr, "failed to allocate %u open handles\n",
			ts_obj_p_cont);
		return -1;
	}

	MPI_Barrier(MPI_COMM_WORLD);

	rc = run_commands(cmds, pf_tests);

	if (ts_in_ult)
		ts_abt_fini();

	if (ts_indices)
		free(ts_indices);
	stride_buf_fini();
	dts_ctx_fini(&ts_ctx);

	MPI_Finalize();

	if (ts_uoids)
		free(ts_uoids);
	if (ts_oids)
		free(ts_oids);
	if (ts_ohs)
		free(ts_ohs);

	return 0;
}
