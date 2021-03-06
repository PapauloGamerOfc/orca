#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <assert.h>

#include "discord.h"


void on_ready(struct discord *client, const struct discord_user *bot) {
  fprintf(stderr, "\n\nLog-Bot succesfully connected to Discord as %s#%s!\n\n",
      bot->username, bot->discriminator);
}

void on_guild_member_add(
  struct discord *client,
  const struct discord_user *bot,
  const uint64_t guild_id, 
  const struct discord_guild_member *member)
{
  printf("%s#%s joined guild %" PRIu64".\n", member->user->username, member->user->discriminator, guild_id);
}

void on_guild_member_update(
  struct discord *client,
  const struct discord_user *bot,
  const uint64_t guild_id, 
  const struct discord_guild_member *member)
{
  printf("%s#%s ", member->user->username, member->user->discriminator);
  if(member->nick && *member->nick) { // is not empty string
    printf("(%s) ", member->nick);
  }
  printf("updated (guild %" PRIu64")\n", guild_id);
}

void on_guild_member_remove(
  struct discord *client,
  const struct discord_user *bot,
  const uint64_t guild_id, 
  const struct discord_user *user)
{
  printf("%s#%s left guild %" PRIu64".\n", user->username, user->discriminator, guild_id);
}

int main(int argc, char *argv[])
{
  const char *config_file;
  if (argc > 1)
    config_file = argv[1];
  else
    config_file = "bot.config";

  discord_global_init();

  struct discord *client = discord_config_init(config_file);
  assert(NULL != client && "Couldn't initialize client");

  discord_add_intents(client, 32767);
  discord_set_on_ready(client, &on_ready);
  discord_set_on_guild_member_add(client, &on_guild_member_add);
  discord_set_on_guild_member_update(client, &on_guild_member_update);
  discord_set_on_guild_member_remove(client, &on_guild_member_remove);

  printf("\n\nThis bot demonstrates how easy it is to listen and log"
         " for events.\n"
         "\nTYPE ANY KEY TO START BOT\n");
  fgetc(stdin); // wait for input

  discord_run(client);

  discord_cleanup(client);

  discord_global_cleanup();
}
