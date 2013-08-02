
#include "fb-messages.pb.h"
#include <string>
#include <iostream>
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "firebuild_common.h"

using namespace std;
using namespace google::protobuf;

static char *fb_conn_string;
static int sigchld_fds[2];
static int child_pid, child_ret = 1;
static io::FileOutputStream * error_fos;
static int debug_level = 0;

static void usage()
{
  cout << "Usage: firebuild [OPTIONS] <BUILD COMMAND>" << endl;
  cout << "Execute BUILD COMMAND with FireBuild™ instrumentation" << endl;
  cout << "" << endl;
  cout << "Mandatory arguments to long options are mandatory for short options too." << endl;
  cout << "   -c --config-file=FILE     use FILE as configuration file" << endl;
  cout << "   -d --debug-level=N        set debugging level to N (0-3, default is 0)" << endl;
  cout << "   -h --help                 show this help" << endl;
  cout << "Exit status:" << endl;
  cout << " exit status of the BUILD COMMAND" << endl;
  cout << " 1  in case of failure" << endl;
}

/**
 * Construct a NULL-terminated array of "NAME=VALUE" environment variables
 * for the build command. The returned stings and array must be free()-d.
 *
 * TODO: detect duplicates
 */
static char** get_sanitized_env()
{
  unsigned int i;
  vector<string> env_v;
  char ** ret_env;
  vector<string>::iterator it;

  // TODO get from config files
  const string pass_through_env_vars[][2] = {{"PATH", ""}, {"SHELL", ""},
					     {"PWD", ""},
					     {"LD_LIBRARY_PATH", ""}};
  string preset_env_vars[][2] = {{"FB_SOCKET", fb_conn_string},
				 {"LD_PRELOAD", "libfbintercept.so"}};

  if (debug_level >= 1) {
    cout << "Passing through environment variables:" << endl;
  }
  for (i = 0; i < sizeof(pass_through_env_vars) / (2 * sizeof(string)); i++) {
    if (NULL  != getenv(pass_through_env_vars[i][0].c_str())) {
      env_v.push_back(pass_through_env_vars[i][0] + "="
		      + getenv(pass_through_env_vars[i][0].c_str()));
      if (debug_level >= 1) {
	cout << " " << env_v.back() << endl;
      }
    }
  }
  if (debug_level >= 1) {
    cout << endl;
    cout << "Setting preset environment variables:" << endl;
  }
  for (i = 0; i < sizeof(preset_env_vars) / (2 * sizeof(string)); i++) {
    env_v.push_back(preset_env_vars[i][0] + "=" + preset_env_vars[i][1]);
    if (debug_level >= 1) {
      cout << " " << env_v.back() << endl;
    }
  }

  if (debug_level >= 1) {
    cout << endl;
  }

  ret_env = static_cast<char**>(malloc(sizeof(char*) * (env_v.size() + 1)));

  it = env_v.begin();
  i = 0;
  while (it != env_v.end()) {
    ret_env[i] = strdup(it->c_str());
    it++;
    i++;
  }
  ret_env[i] = NULL;

  return ret_env;
}

/**
 * signal handler for SIGCHLD
 *
 * It send a 0 to the special file descriptor select is listening on, too.
 */
static void
sigchld_handler (int /*sig */)
{
  char buf[] = {0};
  int status = 0;

  waitpid(child_pid, &status, WNOHANG);
  if (WIFEXITED(status)) {
    child_ret = WEXITSTATUS(status);
    setvbuf(fdopen(sigchld_fds[1],"w"), NULL, _IONBF, 0);
    write(sigchld_fds[1], buf, sizeof(buf));
  } else if (WIFSIGNALED(status)) {
    fprintf(stderr, "Child process has been killed by signal %d",
	     WTERMSIG(status));
    write(sigchld_fds[1], buf, 1);
  }
}

/**
 * Initialize signal handlers
 */
static void
init_signal_handlers(void)
{
  struct sigaction sa;

  sa.sa_handler = sigchld_handler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);

  /* prepare sigchld_fd */

  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("Could not set up signal handler for SIGCHLD.");
    exit(1);
  }
}

/**
 * Process message coming from interceptor
 * @param fb_conn file desctiptor of the connection
 * @return fd_conn can be kept open
 */
bool proc_ic_msg(InterceptorMsg &ic_msg, int fd_conn) {
  if (ic_msg.has_scproc_query()) {
    SupervisorMsg sv_msg;
    ShortCutProcessResp *scproc_resp;
    scproc_resp = sv_msg.mutable_scproc_resp();
    // TODO look up stored result
    if (false /* can shortcut*/) {
      scproc_resp->set_shortcut(true);
      scproc_resp->set_exit_status(0);
    } else {
      scproc_resp->set_shortcut(false);
    }
    fb_send_msg(sv_msg, fd_conn);
  } else if (ic_msg.has_open()) {
  } else if (ic_msg.has_creat()) {
  } else if (ic_msg.has_close()) {
  } else if (ic_msg.has_proc()) {
  } else if (ic_msg.has_exit() ||
	     ic_msg.has_execv()) {
    SupervisorMsg sv_msg;
    sv_msg.set_ack(true);
    fb_send_msg(sv_msg, fd_conn);
  } else if (ic_msg.has_gen_call()) {
  }

  return true;
}


int main(int argc, char* argv[]) {

  char **env_exec, *config_file = NULL;
  int i, c;
  string tempdir;

  // parse options

  while (1) {
    int option_index = 0;
    static struct option long_options[] = {
      {"config-file",  required_argument, 0,  'c' },
      {"debug-level",  required_argument, 0,  'd' },
      {"help",         no_argument,       0,  'h' },
      {0,         0,                 0,  0 }
    };

    c = getopt_long(argc, argv, "c:d:h",
		    long_options, &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 'c':
      config_file = optarg;
      // TODO use config file
      (void)config_file;
      break;

    case 'd':
      debug_level = atoi(optarg);
      if ((debug_level < 0) || (debug_level > 3)) {
	usage();
	exit(1);
      }
      break;

    case 'h':
      usage();
      exit(0);
      break;

    default:
      usage();
      exit(1);
    }
  }
  if (optind >= argc) {
    usage();
    exit(1);
  }

  // Verify that the version of the ProtoBuf library that we linked against is
  // compatible with the version of the headers we compiled against.
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  error_fos = new io::FileOutputStream(STDERR_FILENO);
  {
    fb_conn_string = tempnam(NULL, "firebuild");
  }
  env_exec = get_sanitized_env();

  init_signal_handlers();

  if (pipe(sigchld_fds) == -1) {
    perror("pipe");
    exit(1);
  }

  // run command and handle interceptor messages
  {
    struct sockaddr_un local;
    int len;
    int listener;     // listening socket descriptor

    if ((listener = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
      perror("socket");
      exit(1);
    }

    local.sun_family = AF_UNIX;
    strncpy(local.sun_path, fb_conn_string, sizeof(local.sun_path));
    unlink(local.sun_path);
    len = strlen(local.sun_path) + sizeof(local.sun_family);
    if (bind(listener, (struct sockaddr *)&local, len) == -1) {
      perror("bind");
      exit(1);
    }

    if (listen(listener, 500) == -1) {
      perror("listen");
      exit(1);
    }

    if ((child_pid = fork()) == 0) {
      // intercepted process
      char* argv_exec[argc - optind + 1];

      // we don't need those
      close(sigchld_fds[0]);
      close(sigchld_fds[1]);
      close(listener);
      // create and execute build command
      for (i = 0; i < argc - optind ; i++) {
	argv_exec[i] = argv[optind + i];
      }
      argv_exec[i] = NULL;

      execvpe(argv[optind], argv_exec, env_exec);
      perror("Executing build command failed");
      exit(1);
    } else {
      // supervisor process
      int newfd;        // newly accept()ed socket descriptor
      int fdmax;        // maximum file descriptor number

      fd_set master;    // master file descriptor list
      fd_set read_fds;  // temp file descriptor list for select()

      InterceptorMsg ic_msg;
      SupervisorMsg sv_msg;

      bool child_exited = false;

      uid_t euid = geteuid();

      FD_ZERO(&master);    // clear the master and temp sets
      FD_ZERO(&read_fds);

      // add the listener and and fd listening for child's deeath to the master set
      FD_SET(listener, &master);
      FD_SET(sigchld_fds[0], &master);

      // keep track of the biggest file descriptor
      fdmax = listener; // so far, it's this one
      // main loop for processing interceptor messages
      for(;;) {
	if (child_exited) {
	  break;
	}
        read_fds = master; // copy it
	if (select(fdmax+1, &read_fds, NULL, NULL, NULL) == -1) {
	  if (errno != EINTR) {
	    perror("select");
	    exit(1);
	  } else {
	    break;
	  }
        }

        // run through the existing connections looking for data to read
        for(i = 0; i <= fdmax; i++) {
	  if (FD_ISSET(i, &read_fds)) { // we got one!!
	    if (i == listener) {
	      // handle new connections
	      struct sockaddr_un remote;
	      socklen_t addrlen = sizeof(remote);

	      newfd = accept(listener,
			     (struct sockaddr *)&remote,
			     &addrlen);
	      if (newfd == -1) {
		perror("accept");
	      } else {
		struct ucred creds;
		socklen_t optlen = sizeof(creds);
		getsockopt(newfd, SOL_SOCKET, SO_PEERCRED, &creds, &optlen);
		if (euid != creds.uid) {
		  // someone else started using the socket
		  fprintf(stderr,
			  "Unauthorized connection from pid %d, uid %d, gid %d\n",
			  creds.pid, creds.uid, creds.gid);
		  close(newfd);
		} else {
		  FD_SET(newfd, &master); // add to master set
		  if (newfd > fdmax) {    // keep track of the max
		    fdmax = newfd;
		  }
		}
		// TODO debug
	      }
	    } else if (i == sigchld_fds[0]) {
	      // Our child has exited.
	      // Process remaining messages, then we are done.
	      child_exited = true;
	      continue;
	    } else {
	      // handle data from a client
	      ssize_t nbytes;

	      if ((nbytes = fb_recv_msg(ic_msg, i)) <= 0) {
		// got error or connection closed by client
		if (nbytes == 0) {
		  // connection closed
		  // TODO handle process exit
		  if (debug_level >= 2) {
		    printf("socket %d hung up\n", i);
		  }
		} else {
		  perror("recv");
		}
		close(i); // bye!
		FD_CLR(i, &master); // remove from master set
	      } else {
		if (debug_level >= 2) {
		  cerr << "fd " << i << ": ";
		  TextFormat::Print(ic_msg, error_fos);
		  error_fos->Flush();
		}
		if (!proc_ic_msg(ic_msg, i)) {
		  close(i); // bye!
		  FD_CLR(i, &master); // remove from master set
		}
	      }
	    }
	  }
	}
      }
    }
  }
  // clean up everything
  {
    char* env_str;
    for (i = 0; NULL != (env_str = env_exec[i]); i++) {
      free(env_str);
    }
    free(env_exec);

    unlink(fb_conn_string);
  }

  delete(error_fos);
  free(fb_conn_string);

  // Optional:  Delete all global objects allocated by libprotobuf.
  google::protobuf::ShutdownProtobufLibrary();

  return child_ret;
}

/** wrapper for write() retrying on recoverable errors*/
ssize_t fb_write_buf(int fd, const void *buf, const size_t count)
{
  FB_IO_OP_BUF(write, fd, buf, count, {})
}

/** wrapper for read() retrying on recoverable errors*/
ssize_t fb_read_buf(int fd, const void *buf, const size_t count)
{
  FB_IO_OP_BUF(read, fd, buf, count, {})
}
