Warning: Permanently added 'master_ubss1' (ED25519) to the list of known hosts.

Authorized users only. All activities may be monitored and reported.
Warning: Permanently added 'master_ubss1' (ED25519) to the list of known hosts.

Authorized users only. All activities may be monitored and reported.
OpenSSH_8.8p1, OpenSSL 1.1.1wa  16 Nov 2023
debug1: Reading configuration data /etc/ssh/ssh_config
debug2: checking match for 'final all' host master_ubss2 originally master_ubss2
debug3: /etc/ssh/ssh_config line 56: not matched 'final'
debug2: match not found
debug3: /etc/ssh/ssh_config line 57: Including file /etc/crypto-policies/back-ends/openssh.config depth 0 (parse only)
debug1: Reading configuration data /etc/crypto-policies/back-ends/openssh.config
debug3: gss kex names ok: [gss-curve25519-sha256-,gss-nistp256-sha256-,gss-group14-sha256-,gss-group16-sha512-,gss-gex-sha1-,gss-group14-sha1-,gss-group1-sha1-]
debug3: kex names ok: [curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,diffie-hellman-group-exchange-sha256,diffie-hellman-group14-sha256,diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,diffie-hellman-group-exchange-sha1,diffie-hellman-group14-sha1,diffie-hellman-group1-sha1]
debug1: /etc/ssh/ssh_config line 65: include /etc/ssh/ssh_config.d/*.conf matched no files
debug1: configuration requests final Match pass
debug1: re-parsing configuration
debug1: Reading configuration data /etc/ssh/ssh_config
debug2: checking match for 'final all' host master_ubss2 originally master_ubss2
debug3: /etc/ssh/ssh_config line 56: matched 'final'
debug2: match found
debug3: /etc/ssh/ssh_config line 57: Including file /etc/crypto-policies/back-ends/openssh.config depth 0
debug1: Reading configuration data /etc/crypto-policies/back-ends/openssh.config
debug3: gss kex names ok: [gss-curve25519-sha256-,gss-nistp256-sha256-,gss-group14-sha256-,gss-group16-sha512-,gss-gex-sha1-,gss-group14-sha1-,gss-group1-sha1-]
debug3: kex names ok: [curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,diffie-hellman-group-exchange-sha256,diffie-hellman-group14-sha256,diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,diffie-hellman-group-exchange-sha1,diffie-hellman-group14-sha1,diffie-hellman-group1-sha1]
debug1: /etc/ssh/ssh_config line 65: include /etc/ssh/ssh_config.d/*.conf matched no files
debug3: expanded UserKnownHostsFile '~/.ssh/known_hosts' -> '/home/s2412737/.ssh/known_hosts'
debug3: expanded UserKnownHostsFile '~/.ssh/known_hosts2' -> '/home/s2412737/.ssh/known_hosts2'
debug1: Authenticator provider $SSH_SK_PROVIDER did not resolve; disabling
debug2: resolving "master_ubss2" port 22
debug3: resolve_host: lookup master_ubss2:22
debug3: ssh_connect_direct: entering
debug1: Connecting to master_ubss2 [192.168.90.142] port 22.
debug3: set_sock_tos: set socket 3 IP_TOS 0x48
debug1: Connection established.
debug1: identity file /home/s2412737/.ssh/id_rsa type 0
debug1: identity file /home/s2412737/.ssh/id_rsa-cert type -1
debug1: identity file /home/s2412737/.ssh/id_dsa type -1
debug1: identity file /home/s2412737/.ssh/id_dsa-cert type -1
debug1: identity file /home/s2412737/.ssh/id_ecdsa type -1
debug1: identity file /home/s2412737/.ssh/id_ecdsa-cert type -1
debug1: identity file /home/s2412737/.ssh/id_ecdsa_sk type -1
debug1: identity file /home/s2412737/.ssh/id_ecdsa_sk-cert type -1
debug1: identity file /home/s2412737/.ssh/id_ed25519 type -1
debug1: identity file /home/s2412737/.ssh/id_ed25519-cert type -1
debug1: identity file /home/s2412737/.ssh/id_ed25519_sk type -1
debug1: identity file /home/s2412737/.ssh/id_ed25519_sk-cert type -1
debug1: identity file /home/s2412737/.ssh/id_xmss type -1
debug1: identity file /home/s2412737/.ssh/id_xmss-cert type -1
debug1: Local version string SSH-2.0-OpenSSH_8.8
debug1: Remote protocol version 2.0, remote software version OpenSSH_8.8
debug1: compat_banner: match: OpenSSH_8.8 pat OpenSSH* compat 0x04000000
debug2: fd 3 setting O_NONBLOCK
debug1: Authenticating to master_ubss2:22 as 's2412737'
debug3: record_hostkey: found key type RSA in file /home/s2412737/.ssh/known_hosts:29
debug3: record_hostkey: found key type ED25519 in file /home/s2412737/.ssh/known_hosts:33
debug3: load_hostkeys_file: loaded 2 keys from master_ubss2
debug1: load_hostkeys: fopen /home/s2412737/.ssh/known_hosts2: No such file or directory
debug1: load_hostkeys: fopen /etc/ssh/ssh_known_hosts: No such file or directory
debug1: load_hostkeys: fopen /etc/ssh/ssh_known_hosts2: No such file or directory
debug3: order_hostkeyalgs: have matching best-preference key type ssh-ed25519-cert-v01@openssh.com, using HostkeyAlgorithms verbatim
debug3: send packet: type 20
debug1: SSH2_MSG_KEXINIT sent
debug3: receive packet: type 20
debug1: SSH2_MSG_KEXINIT received
debug2: local client KEXINIT proposal
debug2: KEX algorithms: curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,diffie-hellman-group-exchange-sha256,diffie-hellman-group14-sha256,diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,diffie-hellman-group-exchange-sha1,diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,ext-info-c,kex-strict-c-v00@openssh.com
debug2: host key algorithms: ssh-ed25519-cert-v01@openssh.com,ecdsa-sha2-nistp256-cert-v01@openssh.com,ecdsa-sha2-nistp384-cert-v01@openssh.com,ecdsa-sha2-nistp521-cert-v01@openssh.com,sk-ssh-ed25519-cert-v01@openssh.com,sk-ecdsa-sha2-nistp256-cert-v01@openssh.com,rsa-sha2-512-cert-v01@openssh.com,rsa-sha2-256-cert-v01@openssh.com,ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,sk-ssh-ed25519@openssh.com,sk-ecdsa-sha2-nistp256@openssh.com,rsa-sha2-512,rsa-sha2-256
debug2: ciphers ctos: aes256-gcm@openssh.com,chacha20-poly1305@openssh.com,aes256-ctr,aes256-cbc,aes128-gcm@openssh.com,aes128-ctr,aes128-cbc
debug2: ciphers stoc: aes256-gcm@openssh.com,chacha20-poly1305@openssh.com,aes256-ctr,aes256-cbc,aes128-gcm@openssh.com,aes128-ctr,aes128-cbc
debug2: MACs ctos: hmac-sha2-256-etm@openssh.com,hmac-sha1-etm@openssh.com,umac-128-etm@openssh.com,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha1,umac-128@openssh.com,hmac-sha2-512
debug2: MACs stoc: hmac-sha2-256-etm@openssh.com,hmac-sha1-etm@openssh.com,umac-128-etm@openssh.com,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha1,umac-128@openssh.com,hmac-sha2-512
debug2: compression ctos: none,zlib@openssh.com,zlib
debug2: compression stoc: none,zlib@openssh.com,zlib
debug2: languages ctos: 
debug2: languages stoc: 
debug2: first_kex_follows 0 
debug2: reserved 0 
debug2: peer server KEXINIT proposal
debug2: KEX algorithms: curve25519-sha256,curve25519-sha256@libssh.org,diffie-hellman-group-exchange-sha256,kex-strict-s-v00@openssh.com
debug2: host key algorithms: rsa-sha2-512,rsa-sha2-256,ssh-ed25519
debug2: ciphers ctos: aes128-ctr,aes192-ctr,aes256-ctr,aes128-gcm@openssh.com,aes256-gcm@openssh.com,chacha20-poly1305@openssh.com
debug2: ciphers stoc: aes128-ctr,aes192-ctr,aes256-ctr,aes128-gcm@openssh.com,aes256-gcm@openssh.com,chacha20-poly1305@openssh.com
debug2: MACs ctos: hmac-sha2-512,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha2-256-etm@openssh.com
debug2: MACs stoc: hmac-sha2-512,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha2-256-etm@openssh.com
debug2: compression ctos: none,zlib@openssh.com
debug2: compression stoc: none,zlib@openssh.com
debug2: languages ctos: 
debug2: languages stoc: 
debug2: first_kex_follows 0 
debug2: reserved 0 
debug3: kex_choose_conf: will use strict KEX ordering
debug1: kex: algorithm: curve25519-sha256
debug1: kex: host key algorithm: ssh-ed25519
debug1: kex: server->client cipher: aes256-gcm@openssh.com MAC: <implicit> compression: none
debug1: kex: client->server cipher: aes256-gcm@openssh.com MAC: <implicit> compression: none
debug1: kex: curve25519-sha256 need=32 dh_need=32
debug1: kex: curve25519-sha256 need=32 dh_need=32
debug3: send packet: type 30
debug1: expecting SSH2_MSG_KEX_ECDH_REPLY
debug3: receive packet: type 31
debug1: SSH2_MSG_KEX_ECDH_REPLY received
debug1: Server host key: ssh-ed25519 SHA256:vNqegxOAFfvmh5NDoK1bViKkvyGztkD6M/C/NMeTgLY
debug3: record_hostkey: found key type RSA in file /home/s2412737/.ssh/known_hosts:29
debug3: record_hostkey: found key type ED25519 in file /home/s2412737/.ssh/known_hosts:33
debug3: load_hostkeys_file: loaded 2 keys from master_ubss2
debug1: load_hostkeys: fopen /home/s2412737/.ssh/known_hosts2: No such file or directory
debug1: load_hostkeys: fopen /etc/ssh/ssh_known_hosts: No such file or directory
debug1: load_hostkeys: fopen /etc/ssh/ssh_known_hosts2: No such file or directory
debug1: Host 'master_ubss2' is known and matches the ED25519 host key.
debug1: Found key in /home/s2412737/.ssh/known_hosts:33
debug3: send packet: type 21
debug1: ssh_packet_send2_wrapped: resetting send seqnr 3
debug2: set_newkeys: mode 1
debug1: rekey out after 4294967296 blocks
debug1: SSH2_MSG_NEWKEYS sent
debug1: expecting SSH2_MSG_NEWKEYS
debug3: receive packet: type 21
debug1: ssh_packet_read_poll2: resetting read seqnr 3
debug1: SSH2_MSG_NEWKEYS received
debug2: set_newkeys: mode 0
debug1: rekey in after 4294967296 blocks
debug1: Will attempt key: /home/s2412737/.ssh/id_rsa RSA SHA256:5B6vyaW+ViGOAknp3bYedxxQ0Dlq+fWpqBIxxrRHF2U
debug1: Will attempt key: /home/s2412737/.ssh/id_dsa 
debug1: Will attempt key: /home/s2412737/.ssh/id_ecdsa 
debug1: Will attempt key: /home/s2412737/.ssh/id_ecdsa_sk 
debug1: Will attempt key: /home/s2412737/.ssh/id_ed25519 
debug1: Will attempt key: /home/s2412737/.ssh/id_ed25519_sk 
debug1: Will attempt key: /home/s2412737/.ssh/id_xmss 
debug2: pubkey_prepare: done
debug3: send packet: type 5
debug3: receive packet: type 7
debug1: SSH2_MSG_EXT_INFO received
debug1: kex_input_ext_info: server-sig-algs=<ssh-ed25519,sk-ssh-ed25519@openssh.com,ssh-rsa,rsa-sha2-256,rsa-sha2-512,ssh-dss,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,sk-ecdsa-sha2-nistp256@openssh.com,webauthn-sk-ecdsa-sha2-nistp256@openssh.com,sm2>
debug3: receive packet: type 6
debug2: service_accept: ssh-userauth
debug1: SSH2_MSG_SERVICE_ACCEPT received
debug3: send packet: type 50
debug3: receive packet: type 53
debug3: input_userauth_banner: entering

Authorized users only. All activities may be monitored and reported.
debug3: receive packet: type 51
debug1: Authentications that can continue: publickey,gssapi-keyex,gssapi-with-mic,password,hostbased
debug3: start over, passed a different list publickey,gssapi-keyex,gssapi-with-mic,password,hostbased
debug3: preferred gssapi-with-mic,hostbased,publickey,keyboard-interactive,password
debug3: authmethod_lookup gssapi-with-mic
debug3: remaining preferred: hostbased,publickey,keyboard-interactive,password
debug3: authmethod_is_enabled gssapi-with-mic
debug1: Next authentication method: gssapi-with-mic
debug1: No credentials were supplied, or the credentials were unavailable or inaccessible
No Kerberos credentials available: No KCM server found


debug1: No credentials were supplied, or the credentials were unavailable or inaccessible
No Kerberos credentials available: No KCM server found


debug2: we did not send a packet, disable method
debug3: authmethod_lookup hostbased
debug3: remaining preferred: publickey,keyboard-interactive,password
debug3: authmethod_is_enabled hostbased
debug1: Next authentication method: hostbased
debug3: userauth_hostbased: trying key type ssh-ed25519-cert-v01@openssh.com
debug3: userauth_hostbased: trying key type ecdsa-sha2-nistp256-cert-v01@openssh.com
debug3: userauth_hostbased: trying key type ecdsa-sha2-nistp384-cert-v01@openssh.com
debug3: userauth_hostbased: trying key type ecdsa-sha2-nistp521-cert-v01@openssh.com
debug3: userauth_hostbased: trying key type sk-ssh-ed25519-cert-v01@openssh.com
debug3: userauth_hostbased: trying key type sk-ecdsa-sha2-nistp256-cert-v01@openssh.com
debug3: userauth_hostbased: trying key type rsa-sha2-512-cert-v01@openssh.com
debug3: userauth_hostbased: trying key type rsa-sha2-256-cert-v01@openssh.com
debug3: userauth_hostbased: trying key type ssh-ed25519
debug3: userauth_hostbased: trying key type ecdsa-sha2-nistp256
debug3: userauth_hostbased: trying key type ecdsa-sha2-nistp384
debug3: userauth_hostbased: trying key type ecdsa-sha2-nistp521
debug3: userauth_hostbased: trying key type sk-ssh-ed25519@openssh.com
debug3: userauth_hostbased: trying key type sk-ecdsa-sha2-nistp256@openssh.com
debug3: userauth_hostbased: trying key type rsa-sha2-512
debug3: userauth_hostbased: trying key type rsa-sha2-256
debug1: No more client hostkeys for hostbased authentication.
debug2: we did not send a packet, disable method
debug3: authmethod_lookup publickey
debug3: remaining preferred: keyboard-interactive,password
debug3: authmethod_is_enabled publickey
debug1: Next authentication method: publickey
debug1: Offering public key: /home/s2412737/.ssh/id_rsa RSA SHA256:5B6vyaW+ViGOAknp3bYedxxQ0Dlq+fWpqBIxxrRHF2U
debug3: send packet: type 50
debug2: we sent a publickey packet, wait for reply
debug3: receive packet: type 60
debug1: Server accepts key: /home/s2412737/.ssh/id_rsa RSA SHA256:5B6vyaW+ViGOAknp3bYedxxQ0Dlq+fWpqBIxxrRHF2U
debug3: sign_and_send_pubkey: RSA SHA256:5B6vyaW+ViGOAknp3bYedxxQ0Dlq+fWpqBIxxrRHF2U
debug3: sign_and_send_pubkey: signing using rsa-sha2-256 SHA256:5B6vyaW+ViGOAknp3bYedxxQ0Dlq+fWpqBIxxrRHF2U
debug3: send packet: type 50
debug3: receive packet: type 52
Authenticated to master_ubss2 ([192.168.90.142]:22) using "publickey".
debug1: pkcs11_del_provider: called, provider_id = (null)
debug2: fd 4 setting O_NONBLOCK
debug1: channel 0: new [client-session]
debug3: ssh_session2_open: channel_new: 0
debug2: channel 0: send open
debug3: send packet: type 90
debug1: Requesting no-more-sessions@openssh.com
debug3: send packet: type 80
debug1: Entering interactive session.
debug1: pledge: filesystem full
debug3: receive packet: type 80
debug1: client_input_global_request: rtype hostkeys-00@openssh.com want_reply 0
debug3: client_input_hostkeys: received RSA key SHA256:RAGeIxBPMfXnM8LM7N0Ez4wbwjKOz6Qi0GJZ6ZC9DSg
debug3: client_input_hostkeys: received ED25519 key SHA256:vNqegxOAFfvmh5NDoK1bViKkvyGztkD6M/C/NMeTgLY
debug1: client_input_hostkeys: searching /home/s2412737/.ssh/known_hosts for master_ubss2 / (none)
debug3: hostkeys_foreach: reading file "/home/s2412737/.ssh/known_hosts"
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:1
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:2
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:3
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:4
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:5
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:6
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:7
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:8
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:9
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:10
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:11
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:12
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:13
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:14
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:15
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:16
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:17
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:18
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:19
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:20
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:21
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:22
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:23
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:24
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:25
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:26
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:27
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:28
debug3: hostkeys_find: found ssh-rsa key at /home/s2412737/.ssh/known_hosts:29
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:30
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:31
debug3: hostkeys_find: found ssh-ed25519 key under different name/addr at /home/s2412737/.ssh/known_hosts:32
debug3: hostkeys_find: found ssh-ed25519 key at /home/s2412737/.ssh/known_hosts:33
debug3: hostkeys_find: found ssh-rsa key under different name/addr at /home/s2412737/.ssh/known_hosts:34
debug1: client_input_hostkeys: searching /home/s2412737/.ssh/known_hosts2 for master_ubss2 / (none)
debug1: client_input_hostkeys: hostkeys file /home/s2412737/.ssh/known_hosts2 does not exist
debug3: client_input_hostkeys: 2 server keys: 0 new, 2 retained, 0 incomplete match. 0 to remove
debug1: client_input_hostkeys: no new or deprecated keys from server
debug3: receive packet: type 4
debug1: Remote: /home/s2412737/.ssh/authorized_keys:15: key options: agent-forwarding port-forwarding pty user-rc x11-forwarding
debug3: receive packet: type 4
debug1: Remote: /home/s2412737/.ssh/authorized_keys:15: key options: agent-forwarding port-forwarding pty user-rc x11-forwarding
debug3: receive packet: type 91
debug2: channel_input_open_confirmation: channel 0: callback start
debug2: fd 3 setting TCP_NODELAY
debug3: set_sock_tos: set socket 3 IP_TOS 0x20
debug2: client_session2_setup: id 0
debug1: Sending environment.
debug3: Ignored env SHELL
debug3: Ignored env HISTCONTROL
debug3: Ignored env HOSTNAME
debug3: Ignored env HISTSIZE
debug3: Ignored env PBS_NP
debug3: Ignored env HYDRA_LAUNCHER_EXTRA_ARGS
debug3: Ignored env PBS_TASKNUM
debug3: Ignored env PBS_JOBID
debug3: Ignored env PBS_O_PATH
debug3: Ignored env PBS_NODEFILE
debug3: Ignored env HYDRA_DEBUG
debug3: Ignored env PBS_ENVIRONMENT
debug3: Ignored env PWD
debug3: Ignored env PBS_O_QUEUE
debug3: Ignored env LOGNAME
debug3: Ignored env PBS_JOBCOOKIE
debug3: Ignored env MODULESHOME
debug3: Ignored env MANPATH
debug3: Ignored env PBS_O_HOME
debug3: Ignored env PBS_MOMPORT
debug3: Ignored env PBS_JOBNAME
debug3: Ignored env PBS_NODENUM
debug3: Ignored env HOME
debug1: channel 0: setting env LANG = "en_US.UTF-8"
debug2: channel 0: request env confirm 0
debug3: send packet: type 98
debug3: Ignored env PBS_MICFILE
debug3: Ignored env PBS_O_LANG
debug3: Ignored env PBS_O_LOGNAME
debug3: Ignored env PBS_O_MAIL
debug3: Ignored env PBS_QUEUE
debug3: Ignored env PBS_NUM_NODES
debug3: Ignored env PBS_O_SERVER
debug3: Ignored env USER
debug3: Ignored env ENVIRONMENT
debug3: Ignored env MODULES_RUN_QUARANTINE
debug3: Ignored env LOADEDMODULES
debug3: Ignored env PBS_O_HOST
debug3: Ignored env PBS_GPUFILE
debug3: Ignored env SHLVL
debug3: Ignored env PBS_VNODENUM
debug3: Ignored env PBS_VERSION
debug3: Ignored env PBS_O_SHELL
debug3: Ignored env PATH
debug3: Ignored env MODULEPATH
debug3: Ignored env PBS_O_WORKDIR
debug3: Ignored env MAIL
debug3: Ignored env PBS_NUM_PPN
debug3: Ignored env MODULES_CMD
debug3: Ignored env BASH_FUNC_ml%%
debug3: Ignored env BASH_FUNC_module%%
debug3: Ignored env _
debug1: Sending command: "/usr/local/bin/hydra_pmi_proxy" --control-port 192.168.90.143:32901 --debug --rmk pbs --launcher ssh --demux poll --iface enp1s0 --pgid 0 --retries 10 --usize -2 --proxy-id 1
debug2: channel 0: request exec confirm 1
debug3: send packet: type 98
debug2: channel_input_open_confirmation: channel 0: callback done
debug2: channel 0: open confirm rwindow 0 rmax 32768
debug2: channel 0: rcvd adjust 2097152
debug3: receive packet: type 99
debug2: channel_input_status_confirm: type 99 id 0
debug2: exec request accepted on channel 0
[proxy:0:1@master_ubss2] HYDU_create_process (utils/launch/launch.c:74): execvp error on file /home/s2412737/hello_mpi (No such file or directory)
[proxy:0:1@master_ubss2] HYDU_create_process (utils/launch/launch.c:74): execvp error on file /home/s2412737/hello_mpi (No such file or directory)
[proxy:0:1@master_ubss2] HYDU_create_process (utils/launch/launch.c:74): execvp error on file /home/s2412737/hello_mpi (No such file or directory)
[proxy:0:1@master_ubss2] HYDU_create_process (utils/launch/launch.c:74): execvp error on file /home/s2412737/hello_mpi (No such file or directory)
[proxy:0:1@master_ubss2] HYDU_create_process (utils/launch/launch.c:74): execvp error on file /home/s2412737/hello_mpi (No such file or directory)
[proxy:0:1@master_ubss2] HYDU_create_process (utils/launch/launch.c:74): execvp error on file /home/s2412737/hello_mpi (No such file or directory)
[proxy:0:1@master_ubss2] HYDU_create_process (utils/launch/launch.c:74): execvp error on file /home/s2412737/hello_mpi (No such file or directory)
[proxy:0:1@master_ubss2] HYDU_create_process (utils/launch/launch.c:74): execvp error on file /home/s2412737/hello_mpi (No such file or directory)
debug3: receive packet: type 96
debug2: channel 0: rcvd eof
debug2: channel 0: output open -> drain
debug2: channel 0: obuf empty
debug2: chan_shutdown_write: channel 0: (i0 o1 sock -1 wfd 5 efd 6 [write])
debug2: channel 0: output drain -> closed
debug3: receive packet: type 98
debug1: client_input_channel_req: channel 0 rtype exit-status reply 0
debug3: receive packet: type 98
debug1: client_input_channel_req: channel 0 rtype eow@openssh.com reply 0
debug2: channel 0: rcvd eow
debug2: chan_shutdown_read: channel 0: (i0 o3 sock -1 wfd 4 efd 6 [write])
debug2: channel 0: input open -> closed
debug3: receive packet: type 97
debug2: channel 0: rcvd close
debug3: channel 0: will not send data after close
debug2: channel 0: almost dead
debug2: channel 0: gc: notify user
debug2: channel 0: gc: user detached
debug2: channel 0: send close
debug3: send packet: type 97
debug2: channel 0: is dead
debug2: channel 0: garbage collecting
debug1: channel 0: free: client-session, nchannels 1
debug3: channel 0: status: The following connections are open:
  #0 client-session (t4 r0 i3/0 o3/0 e[write]/0 fd -1/-1/6 sock -1 cc -1)

debug3: send packet: type 1
Transferred: sent 3488, received 2784 bytes, in 0.2 seconds
Bytes per second: sent 22891.0, received 18270.8
debug1: Exit status 0
[mpiexec@master_ubss3] HYDU_sock_write (utils/sock/sock.c:294): write error (Bad file descriptor)
[mpiexec@master_ubss3] HYD_pmcd_pmiserv_send_signal (pm/pmiserv/pmiserv_cb.c:177): unable to write data to proxy
[mpiexec@master_ubss3] ui_cmd_cb (pm/pmiserv/pmiserv_pmci.c:79): unable to send signal downstream
[mpiexec@master_ubss3] HYDT_dmxu_poll_wait_for_event (tools/demux/demux_poll.c:76): callback returned error status
[mpiexec@master_ubss3] HYD_pmci_wait_for_completion (pm/pmiserv/pmiserv_pmci.c:198): error waiting for event
[mpiexec@master_ubss3] main (ui/mpich/mpiexec.c:340): process manager error waiting for completion
