#include "libmcu/jobqueue.h"

#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>

#include "libmcu/list.h"
#include "libmcu/logging.h"

#if !defined(JOBQUEUE_DEFAULT_STACK_SIZE)
#define JOBQUEUE_DEFAULT_STACK_SIZE		3072
#endif
#if !defined(JOBQUEUE_DEFAULT_MIN_THREADS)
#define JOBQUEUE_DEFAULT_MIN_THREADS		1
#endif
#if !defined(JOBQUEUE_DEFAULT_MAX_THREADS)
#define JOBQUEUE_DEFAULT_MAX_THREADS		1
#endif
#if !defined(JOBQUEUE_DEFAULT_PRIORITY)
#define JOBQUEUE_DEFAULT_PRIORITY		5
#endif

typedef enum {
	JOB_STATE_INITIALIZED			= 0,
	JOB_STATE_SCHEDULED,
	JOB_STATE_COMPLETED,
	JOB_STATE_DELETED,
	JOB_STATE_UNKNOWN,
} job_state_t;

struct jobqueue {
	pthread_mutex_t lock;
	struct list job_list;
	sem_t job_queue;
	jobqueue_attr_t attr;
	uint8_t active_threads;
	unsigned int max_concurrent_jobs;
};

struct job {
	struct list entry;
	struct list *pool;
	job_state_t state;
	job_callback_t callback;
	void *context;
};

static inline uint8_t job_count_internal(jobqueue_t *pool)
{
	uint8_t count = 0;
	struct list *p;

	list_for_each(p, &pool->job_list) {
		count++;
	}

	return count;
}

static inline void job_delete_internal(jobqueue_t *pool, struct job *job)
{
	if (job->state != JOB_STATE_SCHEDULED) {
		return;
	}

	struct list *p;
	list_for_each(p, &pool->job_list) {
		if (p == &job->entry) {
			list_del(p, &pool->job_list);
			job->state = JOB_STATE_DELETED;
			break;
		}
	}
}

static bool jobqueue_process(jobqueue_t *pool)
{
	bool busy = true;
	struct job *job = NULL;

	sem_wait(&pool->job_queue);

	pthread_mutex_lock(&pool->lock);
	{
		uint8_t njobs_waiting = job_count_internal(pool);

		if (njobs_waiting) {
			job = list_entry(pool->job_list.next, struct job, entry);
			list_del(&job->entry, &pool->job_list);
			job->state = JOB_STATE_COMPLETED;
		}

		if (njobs_waiting <= pool->active_threads
				&& pool->active_threads > pool->attr.min_threads) {
			pool->active_threads--;
			busy = false;
		}
	}
	pthread_mutex_unlock(&pool->lock);

	if (job && job->callback) {
		job->callback(job->context);
	}

	return busy;
}

static void *jobqueue_task(void *e)
{
	while (jobqueue_process(e));

#if !defined(UNITTEST)
	pthread_exit(NULL);
#endif
	return NULL;
}

static inline job_error_t job_schedule_internal(jobqueue_t *pool, struct job *job)
{
	uint8_t nr_jobs = job_count_internal(pool);
	if (nr_jobs >= pool->max_concurrent_jobs) {
		return JOB_FULL;
	}

	if (job->state != JOB_STATE_SCHEDULED) {
		list_add_tail(&job->entry, &pool->job_list);
		job->state = JOB_STATE_SCHEDULED;
		sem_post(&pool->job_queue);
	}

	if (nr_jobs >= pool->active_threads
			&& pool->active_threads < pool->attr.max_threads) {
		pthread_t thread;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, pool->attr.stack_size_bytes);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&thread, &attr, jobqueue_task, pool) == 0) {
			pool->active_threads++;
		}
	}

	return JOB_SUCCESS;
}

jobqueue_t *jobqueue_create(unsigned int max_concurrent_jobs)
{
	jobqueue_t *pool;

	if (!(pool = calloc(1, sizeof(*pool)))) {
		goto out_err;
	}
	if (sem_init(&pool->job_queue, 0, 0) != 0) {
		goto out_free_pool;
	}

	list_init(&pool->job_list);
	pthread_mutex_init(&pool->lock, NULL);
	pool->active_threads = 0;
	pool->max_concurrent_jobs = max_concurrent_jobs;
	pool->attr = (jobqueue_attr_t) {
		.stack_size_bytes = JOBQUEUE_DEFAULT_STACK_SIZE,
		.min_threads = JOBQUEUE_DEFAULT_MIN_THREADS,
		.max_threads = JOBQUEUE_DEFAULT_MAX_THREADS,
		.priority = JOBQUEUE_DEFAULT_PRIORITY,
	};

	return pool;
out_free_pool:
	free(pool);
out_err:
	return NULL;
}

job_error_t jobqueue_set_attr(jobqueue_t *pool, const jobqueue_attr_t *attr)
{
	if (!pool || !attr) {
		return JOB_INVALID_PARAM;
	}

	pthread_mutex_lock(&pool->lock);
	{
		pool->attr = *attr;
	}
	pthread_mutex_unlock(&pool->lock);

	return JOB_SUCCESS;
}

job_error_t jobqueue_destroy(jobqueue_t *pool)
{
	if (!pool) {
		return JOB_INVALID_PARAM;
	}

	uint8_t jobs_left = job_count(pool);
	if (jobs_left != 0) {
		warn("%u jobs to be run exist", jobs_left);
	}

	pthread_mutex_lock(&pool->lock);
	{
		sem_destroy(&pool->job_queue);
	}
	pthread_mutex_unlock(&pool->lock);

	free(pool);

	return JOB_SUCCESS;
}

job_error_t job_init(jobqueue_t *pool, job_t *job,
		job_callback_t callback, void *context)
{
	if (!pool || !job) {
		return JOB_INVALID_PARAM;
	}

	struct job *p = (struct job *)job;

	p->pool = &pool->job_list;
	p->callback = callback;
	p->context = context;
	p->state = JOB_STATE_INITIALIZED;

	return JOB_SUCCESS;
}

job_error_t job_delete(jobqueue_t *pool, job_t *job)
{
	if (!pool || !job) {
		return JOB_INVALID_PARAM;
	}

	pthread_mutex_lock(&pool->lock);
	{
		job_delete_internal(pool, (struct job *)job);
	}
	pthread_mutex_unlock(&pool->lock);

	return JOB_SUCCESS;
}

job_error_t job_schedule(jobqueue_t *pool, job_t *job)
{
	if (!pool || !job) {
		return JOB_INVALID_PARAM;
	}

	job_error_t err;

	pthread_mutex_lock(&pool->lock);
	{
		err = job_schedule_internal(pool, (struct job *)job);
	}
	pthread_mutex_unlock(&pool->lock);

	return err;
}

uint8_t job_count(jobqueue_t *pool)
{
	uint8_t count;

	pthread_mutex_lock(&pool->lock);
	{
		count = job_count_internal(pool);
	}
	pthread_mutex_unlock(&pool->lock);

	int semcnt = 0;
	sem_getvalue(&pool->job_queue, &semcnt);
	if (count != (uint8_t)semcnt) {
		error("count doesn't match %d - %d", count, semcnt);
	}

	return count;
}

const char *job_stringify_error(job_error_t error_code)
{
	switch (error_code) {
	case JOB_SUCCESS:
		return "success";
	case JOB_INVALID_PARAM:
		return "invalid parameters";
	case JOB_FULL:
		return "no room for a new job";
	case JOB_ERROR:
	default:
		break;
	}

	return "unknown error";
}
