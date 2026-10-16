#title[Keeping Invite Codes Alive on Multiplayer Servers]

#figure(
  image("../../static/images/invite-code-alive.png", width: 80%),
)

Once this option is enabled, during a multiplayer game with invite codes in use, the game will periodically verify with the OpenTTD multiplayer coordination server whether its own invite code is still valid, preventing the server from becoming unreachable due to invite code issues.
