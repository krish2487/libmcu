#include "libmcu/pubsub.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>

#include "libmcu/compiler.h"
#include "libmcu/logging.h"

#if !defined(PUBSUB_MIN_SUBSCRIPTION_CAPACITY)
#define PUBSUB_MIN_SUBSCRIPTION_CAPACITY		4
#endif

/* NOTE: It sets the least significant bit of `subscriber->context` to
 * differentiate static subscriber from one created dynamically. */
#define GET_SUBSCRIBER_CONTEXT(obj)			\
	(void *)((uintptr_t)(obj)->context & ~1UL)
#define IS_SUBSCRIBER_STATIC(obj)			\
	((uintptr_t)(obj)->context & 1UL)
#define GET_CONTEXT_STATIC(ctx)				\
	(void *)((uintptr_t)(ctx) | 1UL)

typedef struct {
	const char *topic_filter;
	pubsub_callback_t callback;
	void *context;
	intptr_t _placeholder_for_compatibility;
} subscribe_t;
LIBMCU_STATIC_ASSERT(sizeof(subscribe_t) == sizeof(pubsub_subscribe_t),
	"The size of public and private subscribe data type must be the same.");

static struct {
	pthread_mutex_t subscriptions_lock;
	const subscribe_t **subscriptions; // dynamically allocated array
	uint8_t max_subscriptions;
	uint8_t nr_subscriptions;
} m;

static bool is_filtering_done(const char * const filter, const char * const topic)
{
	if (*filter != '\0') {
		return false;
	}
	if (*topic != '\0' && *topic != '/') {
		return false;
	}
	if (*topic != '\0' && topic[1] != '\0') {
		return false;
	}
	return true;
}

static bool is_topic_matched_with(const char * const filter,
		const char * const topic)
{
	int i = 0;
	int j = 0;

	while (filter[i] != '\0' && topic[j] != '\0') {
		if (filter[i] == '#') {
			return true;
		}
		if (filter[i] == '+') {
			while (filter[i] != '/') {
				if (filter[i] == '\0') {
					return false;
				}
				i++;
			}
			while (topic[j] != '\0' && topic[j] != '/') {
				j++;
			}
		}

		if (filter[i] != topic[j]) {
			return false;
		}

		i += 1;
		j += 1;
	}

	return is_filtering_done(&filter[i], &topic[j]);
}

static unsigned int count_subscribers(const char *topic)
{
	uint8_t count = 0;

	for (uint8_t i = 0; i < m.max_subscriptions; i++) {
		const subscribe_t *sub = m.subscriptions[i];
		if (sub == NULL) {
			continue;
		}
		if (is_topic_matched_with(sub->topic_filter, topic)) {
			count++;
		}
	}

	return (unsigned int)count;
}

static size_t copy_subscriptions(const subscribe_t **new_subs,
		const subscribe_t **old_subs, size_t n)
{
	size_t count = 0;

	for (size_t i = 0; i < n; i++) {
		if (old_subs[i] != NULL) {
			new_subs[count++] = old_subs[i];
		}
	}

	return count;
}

static bool expand_subscription_capacity(void)
{
	uint8_t capacity = m.max_subscriptions;
	uint8_t new_capacity = (uint8_t)(capacity * 2U);
	assert((uint16_t)capacity * 2U < (1U << 8));

	const subscribe_t **new_subs = (const subscribe_t **)
		calloc(new_capacity, sizeof(subscribe_t *));
	if (new_subs == NULL) {
		return false;
	}

	const subscribe_t **old_subs = m.subscriptions;
	uint8_t count = (uint8_t)copy_subscriptions(new_subs, old_subs,
			(size_t)capacity);
	assert(count < new_capacity);
	assert(count == m.nr_subscriptions);

	m.max_subscriptions = new_capacity;
	m.subscriptions = new_subs;
	free(old_subs);

	info("Expanded from %u to %u", capacity, new_capacity);
	return true;
}

static void shrink_subscription_capacity(void)
{
	uint8_t capacity = m.max_subscriptions;
	uint8_t new_capacity = capacity / 2U;

	if (capacity <= PUBSUB_MIN_SUBSCRIPTION_CAPACITY) {
		return;
	}
	if (m.nr_subscriptions * 2U >= capacity) {
		return;
	}

	const subscribe_t **new_subs = (const subscribe_t **)
		calloc(new_capacity, sizeof(subscribe_t *));
	if (new_subs == NULL) {
		return;
	}

	const subscribe_t **old_subs = m.subscriptions;
	uint8_t count = (uint8_t)copy_subscriptions(new_subs, old_subs,
			(size_t)capacity);
	assert(count < new_capacity);
	assert(count == m.nr_subscriptions);

	m.max_subscriptions = new_capacity;
	m.subscriptions = new_subs;
	free(old_subs);

	info("Shrunken from %u to %u", capacity, new_capacity);
}

static bool register_subscription(const subscribe_t *sub)
{
	if (m.nr_subscriptions >= m.max_subscriptions) {
		if (!expand_subscription_capacity()) {
			error("can't expand");
			return false;
		}
	}

	for (uint8_t i = 0; i < m.max_subscriptions; i++) {
		if (m.subscriptions[i] == NULL) {
			m.subscriptions[i] = sub;
			m.nr_subscriptions++;
			debug("%p added for \"%s\"", sub, sub->topic_filter);
			return true;
		}
	}

	assert(0); // can not reach down here
}

static bool unregister_subscription(const subscribe_t *sub)
{
	for (uint8_t i = 0; i < m.max_subscriptions; i++) {
		if (m.subscriptions[i] == sub) {
			m.subscriptions[i] = NULL;
			m.nr_subscriptions--;
			shrink_subscription_capacity();
			debug("%p removed from \"%s\"", sub, sub->topic_filter);
			return true;
		}
	}

	warn("%p for \"%s\" not found", sub, sub->topic_filter);
	return false;
}

static void publish_internal(const char *topic, const void *msg, size_t msglen)
{
	for (uint8_t i = 0; i < m.max_subscriptions; i++) {
		const subscribe_t *sub = m.subscriptions[i];
		if (sub == NULL) {
			continue;
		}
		if (is_topic_matched_with(sub->topic_filter, topic)) {
			sub->callback(GET_SUBSCRIBER_CONTEXT(sub), msg, msglen);
		}
	}
}

static void subscriptions_lock(void)
{
	pthread_mutex_lock(&m.subscriptions_lock);
}

static void subscriptions_unlock(void)
{
	pthread_mutex_unlock(&m.subscriptions_lock);
}

static subscribe_t *subscribe_core(subscribe_t *sub, const char *topic_filter,
		pubsub_callback_t cb, void *context)
{
	if ((topic_filter == NULL) || (cb == NULL)) {
		return NULL;
	}

	sub->topic_filter = topic_filter;
	sub->callback = cb;
	sub->context = context;

	bool result = false;
	subscriptions_lock();
	{
		result = register_subscription(sub);
	}
	subscriptions_unlock();
	if (!result) {
		return NULL;
	}

	info("Subscribe to \"%s\"", topic_filter);

	return sub;
}

pubsub_error_t pubsub_create(const char *topic)
{
	unused(topic);
	return PUBSUB_SUCCESS;
}

pubsub_error_t pubsub_destroy(const char *topic)
{
	unused(topic);
	return PUBSUB_SUCCESS;
}

pubsub_error_t pubsub_publish(const char *topic, const void *msg, size_t msglen)
{
	if (!topic || !msg || !msglen) {
		return PUBSUB_INVALID_PARAM;
	}

	subscriptions_lock();
	{
		publish_internal(topic, msg, msglen);
	}
	subscriptions_unlock();

	debug("Publish to \"%s\"", topic);

	return PUBSUB_SUCCESS;
}

pubsub_subscribe_t *pubsub_subscribe_static(pubsub_subscribe_t *obj,
		const char *topic_filter, pubsub_callback_t cb, void *context)
{
	return (pubsub_subscribe_t *)
		subscribe_core((subscribe_t *)obj, topic_filter, cb,
				GET_CONTEXT_STATIC(context));
}

pubsub_subscribe_t *pubsub_subscribe(const char *topic_filter,
		pubsub_callback_t cb, void *context)
{
	subscribe_t *obj = (subscribe_t *)calloc(1, sizeof(*obj));

	if (obj == NULL) {
		return NULL;
	}

	if (subscribe_core(obj, topic_filter, cb, context) == NULL) {
		free(obj);
		return NULL;
	}

	return (pubsub_subscribe_t *)obj;
}

pubsub_error_t pubsub_unsubscribe(pubsub_subscribe_t *obj)
{
	subscribe_t *sub = (subscribe_t *)obj;

	if (sub == NULL || sub->topic_filter == NULL) {
		return PUBSUB_INVALID_PARAM;
	}

	bool result = false;
	subscriptions_lock();
	{
		result = unregister_subscription(sub);
	}
	subscriptions_unlock();

	if (!result) {
		return PUBSUB_NO_EXIST_SUBSCRIBER;
	}

	info("Unsubscribe from \"%s\"", sub->topic_filter);

	if (!IS_SUBSCRIBER_STATIC(sub)) {
		free(sub);
	}

	return PUBSUB_SUCCESS;
}

int pubsub_count(const char *topic)
{
	if (topic == NULL) {
		return PUBSUB_INVALID_PARAM;
	}

	int count = 0;
	subscriptions_lock();
	{
		count = (int)count_subscribers(topic);
	}
	subscriptions_unlock();

	return count;
}

void pubsub_init(void)
{
	pthread_mutex_init(&m.subscriptions_lock, NULL);
	m.nr_subscriptions = 0;
	m.max_subscriptions = PUBSUB_MIN_SUBSCRIPTION_CAPACITY;
	m.subscriptions = (const subscribe_t **)calloc(m.max_subscriptions,
					sizeof(subscribe_t *));
	assert(m.subscriptions != NULL);
}

void pubsub_deinit(void)
{
	subscriptions_lock();
	for (uint8_t i = 0; i < m.max_subscriptions; i++) {
		const subscribe_t *sub = m.subscriptions[i];
		if (sub == NULL) {
			continue;
		}
		if (!IS_SUBSCRIBER_STATIC(sub)) {
			intptr_t *p = (intptr_t *)&sub;
			free((void *)*p);
		}
	}
	free(m.subscriptions);
}

const char *pubsub_stringify_error(pubsub_error_t err)
{
	switch (err) {
	case PUBSUB_SUCCESS:
		return "success";
	case PUBSUB_ERROR:
		return "error";
	case PUBSUB_EXIST_TOPIC:
		return "exist topic";
	case PUBSUB_NO_EXIST_TOPIC:
		return "no exist topic";
	case PUBSUB_NO_MEMORY:
		return "no memory";
	case PUBSUB_INVALID_PARAM:
		return "invalid parameters";
	case PUBSUB_EXIST_SUBSCRIBER:
		return "exist subscriber";
	case PUBSUB_NO_EXIST_SUBSCRIBER:
		return "no exist subscriber";
	default:
		break;
	}
	return "unknown error";
}
