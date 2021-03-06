#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h> // for bucket synchronization

#include "discord.h"
#include "discord-internal.h"
#include "orka-utils.h"


/* See:
https://discord.com/developers/docs/topics/rate-limits#rate-limits */


static struct discord_bucket*
bucket_init(char hash[])
{
  struct discord_bucket *new_bucket = calloc(1, sizeof *new_bucket);
  int ret = snprintf(new_bucket->hash, sizeof(new_bucket->hash), "%s", hash);
  ASSERT_S(ret < sizeof(new_bucket->hash), "Out of bounds write attempt");
  if (pthread_mutex_init(&new_bucket->lock, NULL))
    ERR("Couldn't initialize pthread mutex");
  if (pthread_cond_init(&new_bucket->cond, NULL))
    ERR("Couldn't initialize pthread cond");

  return new_bucket;
}

static void
bucket_cleanup(struct discord_bucket *bucket) 
{
  pthread_mutex_destroy(&bucket->lock);
  pthread_cond_destroy(&bucket->cond);
  free(bucket);
}

/* works like strcmp, but will check if route matches major 
 *  parameter criteria too */
static int
routecmp(char route[], char node_route[])
{
  int ret = strcmp(route, node_route);
  if (0 == ret) return 0;

  /* check if fits major parameter criteria */
  if (strstr(route, "/channels/%") && strstr(node_route, "/channels/%"))
    return 0;
  if (strstr(route, "/guilds/%") && strstr(node_route, "/guilds/%"))
    return 0;
  if (strstr(route, "/webhook/%") && strstr(node_route, "/webhook/%"))
    return 0;

  return ret; //couldn't find any match, return strcmp diff value
}

void
discord_buckets_init(struct discord_adapter *adapter) {
  avl_initialize(&adapter->ratelimit.routes, (avl_comparator_t)&routecmp, &free);
}

static void route_free(void *route, void *noop) { free(route); }

/* clean routes and buckets */
void
discord_buckets_cleanup(struct discord_adapter *adapter)
{ 
  avl_destroy(&adapter->ratelimit.routes, &route_free);
  for (size_t i=0; i < adapter->ratelimit.num_buckets; ++i) {
    bucket_cleanup(adapter->ratelimit.bucket_pool[i]);
  }
  free(adapter->ratelimit.bucket_pool);
}

/* sleep cooldown for a connection within this bucket in milliseconds */
void
discord_bucket_try_cooldown(struct discord_bucket *bucket)
{
  if (!bucket) {
    log_debug("[?] Missing 'bucket', skipping cooldown");
    return; /* EARLY RETURN */
  }

  pthread_mutex_lock(&bucket->lock);
  ++bucket->busy;

  // wait for a while if busy requests reach threshold
  /// @todo? add pthread_broadcast() to avoid zombie threads
  if (bucket->busy > bucket->remaining) {
    log_debug("[%s] Reach live transfers threshold (%d)\n" \
              "Transfer is now locked in queue.", bucket->hash, bucket->remaining);
    pthread_cond_wait(&bucket->cond, &bucket->lock);
    log_debug("[%s] Transfer unlocked from queue", bucket->hash);
  }
  if (bucket->remaining) {
    --bucket->remaining;
    log_debug("[%s] %d remaining transfers before cooldown", bucket->hash, bucket->remaining);
    pthread_mutex_unlock(&bucket->lock);
    return; /* EARLY RETURN */
  }

  u64_unix_ms_t curr_tstamp = orka_timestamp_ms();
  int64_t delay_ms = (int64_t)(bucket->reset_tstamp - curr_tstamp);
  if (delay_ms <= 0) { //no delay needed
    log_debug("[%s] Skipping cooldown because current timestamp"
              " exceeds bucket reset timestamp\n\t"
              "Reset At:\t%"PRIu64"\n\t"
              "Current:\t%"PRIu64"\n\t"
              "Delay:\t%"PRId64, 
              bucket->hash, bucket->reset_tstamp, curr_tstamp, delay_ms);
    pthread_mutex_unlock(&bucket->lock);
    return; /* EARLY RETURN */
  }

  if (delay_ms > bucket->reset_after_ms) //don't delay in excess
    delay_ms = bucket->reset_after_ms;

  log_trace("[%s] RATELIMITING (wait %"PRId64" ms)", bucket->hash, delay_ms);

  orka_sleep_ms(delay_ms); //sleep for delay amount (if any)

  pthread_mutex_unlock(&bucket->lock);
}

/* attempt to find a bucket associated with this route */
struct discord_bucket*
discord_bucket_try_get(struct discord_adapter *adapter, char route[]) 
{
  log_debug("[?] Attempt to find matching bucket for '%s'", route);
  struct discord_bucket *ret = avl_search(&adapter->ratelimit.routes, route);
  if (!ret)
    log_debug("[?] Couldn't match bucket to '%s', will attempt to create a new one", route);
  else
    log_debug("[%s] Found a match!", ret->hash);

  return ret;
}

/* attempt to parse rate limit's header fields to the bucket
 *  linked with the connection which was performed */
static void
parse_ratelimits(struct discord_bucket *bucket, struct ua_info *info)
{ 
  pthread_mutex_lock(&bucket->lock);
  --bucket->busy;

  if (ORCA_OK == info->code && bucket->update_tstamp < info->req_tstamp) 
  {
    bucket->update_tstamp = info->req_tstamp;

    char *str; // fetch header value as string
    if ((str = ua_info_respheader_field(info, "x-ratelimit-reset")))
      bucket->reset_tstamp = 1000 * strtod(str, NULL);
    if ((str = ua_info_respheader_field(info, "x-ratelimit-remaining")))
      bucket->remaining =  strtol(str, NULL, 10);
    if ((str = ua_info_respheader_field(info, "x-ratelimit-reset-after")))
      bucket->reset_after_ms = 1000 * strtod(str, NULL);

    log_debug("\n\t[%s]\n\t"         \
              "reset_tstamp: %"PRIu64"\n\t" \
              "remaining: %d\n\t"           \
              "reset_after_ms: %"PRId64,    \
              bucket->hash, 
              bucket->reset_tstamp, 
              bucket->remaining, 
              bucket->reset_after_ms);
  }
  else {
    log_debug("[%s] Couldn't complete request or" \
              " request timestamp is older than bucket last update", bucket->hash);
  }

  pthread_cond_signal(&bucket->cond);
  pthread_mutex_unlock(&bucket->lock);
}

/* Attempt to find/link a route between route and a client bucket by
 *  comparing the hash retrieved from response header with discovered
 *  buckets hashes
 * If no match is found then a new bucket is created and linked to the
 *  route*/
static void
match_route(struct discord_adapter *adapter, char route[], struct ua_info *info)
{
  char *hash = ua_info_respheader_field(info, "x-ratelimit-bucket");
  if (!hash) {
    log_debug("[?] Missing bucket-hash from response header," \
              " route '%s' can't be assigned to a bucket", route);
    return;
  }

  struct discord_bucket **bucket_pool = adapter->ratelimit.bucket_pool;
  struct discord_bucket *bucket=NULL;

  //attempt to match hash to client bucket hashes
  for (size_t i=0; i < adapter->ratelimit.num_buckets; ++i) {
    if (STREQ(hash, bucket_pool[i]->hash)) {
      bucket = bucket_pool[i];
      break;
    }
  }

  //couldn't find match, create new bucket
  if (!bucket) 
  {
    ++adapter->ratelimit.num_buckets; //increments client buckets

    bucket_pool = realloc(bucket_pool, adapter->ratelimit.num_buckets * sizeof(struct discord_bucket*));
    adapter->ratelimit.bucket_pool = bucket_pool;

    bucket = bucket_init(hash);
    bucket_pool[adapter->ratelimit.num_buckets-1] = bucket;
  }

  //add new route to tree and update its bucket ratelimit fields
  log_debug("[%s] New route '%s'", bucket->hash, route);
  avl_insert(&adapter->ratelimit.routes, strdup(route), bucket);
  parse_ratelimits(bucket, info);
}

/* Attempt to build and/or update bucket's rate limiting information. */
void
discord_bucket_build(struct discord_adapter *adapter, struct discord_bucket *bucket, char route[], struct ua_info *info)
{
  /* no bucket means first time using this route.  attempt to 
   *  establish a route between it and a bucket via its unique hash 
   *  (will create a new bucket if it can't establish a route) */
  if (!bucket)
    match_route(adapter, route, info);
  else // update the bucket rate limit values
    parse_ratelimits(bucket, info);
}
