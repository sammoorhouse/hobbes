
#include <hobbes/storage.H>
#include <hobbes/util/str.H>
#include <hobbes/util/codec.H>
#include <hobbes/util/time.H>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <stdexcept>

#include "local.H"
#include "batchsend.H"
#include "batchrecv.H"
#include "session.H"
#include "netio.H"

namespace hog {

// we can run in one of three modes
struct RunMode {
  enum type { local, batchsend, batchrecv };
  type                  t;
  std::string           dir;
  std::string           groupServerDir;
  std::set<std::string> groups;
  bool                  consolidate;

  // batchsend
  size_t      clevel;
  size_t      batchsendsize;
  long        batchsendtime;
  std::string sendto;

  // batchrecv
  std::string localport;
};

std::ostream& operator<<(std::ostream& o, const std::set<std::string>& xs) {
  o << "{";
  if (xs.size() > 0) {
    auto x = xs.begin();
    o << "\"" << *x << "\"";
    ++x;
    for (; x != xs.end(); ++x) {
      o << ", \"" << *x << "\"";
    }
  }
  o << "}";
  return o;
}

std::ostream& operator<<(std::ostream& o, const RunMode& m) {
  switch (m.t) {
  case RunMode::local:
    o << "|local={ dir=\"" << m.dir << "\", serverDir=\"" << m.groupServerDir << "\", groups=" << m.groups << " }|";
    break;
  case RunMode::batchsend:
    o << "|batchsend={ dir=\"" << m.dir << "\", serverDir=\"" << m.groupServerDir << "\", clevel=" << m.clevel << ", batchsendsize=" << m.batchsendsize << "B, sendto=" << m.sendto << ", groups=" << m.groups << " }|";
    break;
  case RunMode::batchrecv:
    o << "|batchrecv={ dir=\"" << m.dir << "\", localport=" << m.localport << " }|";
    break;
  }
  return o;
}

size_t sizeInBytes(const std::string& s) {
  if (hobbes::str::endsWith(s, "GB")) {
    return 1024 * 1024 * 1024 * hobbes::str::to<size_t>(s);
  } else if (hobbes::str::endsWith(s, "MB")) {
    return 1024 * 1024 * hobbes::str::to<size_t>(s);
  } else if (hobbes::str::endsWith(s, "KB")) {
    return 1024 * hobbes::str::to<size_t>(s);
  } else {
    return hobbes::str::to<size_t>(s);
  }
}

void showUsage() {
  std::cout
  <<
    "hog : record structured data locally or to a remote process\n"
    "\n"
    "  usage: hog [-d <dir>] [-g group+] [-p t s host:port] [-s port] [-c] [-m <dir>]\n"
    "where\n"
    "  -d <dir>         : decides where structured data (or temporary data) is stored\n"
    "  -g group+        : decides which data to record from memory on this machine\n"
    "  -p t s host:port : decides to send data to a remote process every t time units or every s uncompressed bytes written\n"
    "  -s port          : decides to receive data on the given port\n"
    "  -c               : decides to store equally-typed data across processes in a single file\n"
    "  -m <dir>         : decides where to place the domain socket for producer registration (default: " << hobbes::storage::defaultStoreDir() << ")\n"
  << std::endl;
}

RunMode config(int argc, const char** argv) {
  RunMode r;
  r.t              = RunMode::local;
  r.dir            = "./$GROUP/$DATE/data";
  r.groupServerDir = hobbes::storage::defaultStoreDir();
  r.consolidate    = false;

  if (argc == 1) {
    showUsage();
    exit(0);
  }

  for (size_t i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    
    if (arg == "-?" || arg == "--help") {
      showUsage();
      exit(0);
    } else if (arg == "-d") {
      ++i;
      if (i < argc) {
        r.dir = argv[i];
      } else {
        throw std::runtime_error("no directory specified");
      }
    } else if (arg == "-g") {
      ++i;
      while (i < argc && argv[i][0] != '-') {
        r.groups.insert(argv[i]);
        ++i;
      }
      --i;
    } else if (arg == "-p") {
      if (i+3 < argc) {
        r.clevel = 6;

        ++i;
        r.batchsendtime = hobbes::readTimespan(argv[i]);

        ++i;
        r.batchsendsize = sizeInBytes(argv[i]);

        ++i;
        r.sendto = argv[i];

        r.t = RunMode::batchsend;
      } else {
        throw std::runtime_error("need delay time, max size, and remote host:port to push data");
      }
    } else if (arg == "-s") {
      ++i;
      if (i < argc) {
        r.localport = argv[i];
      } else {
        throw std::runtime_error("need port to receive remote data");
      }
      r.t = RunMode::batchrecv;
    } else if (arg == "-c") {
      r.consolidate = true;
    } else if (arg == "-m") {
      ++i;
      if (i < argc) {
        r.groupServerDir = argv[i];
      } else {
        throw std::runtime_error("need domain socket directory for producer registration");
      }
    } else {
      throw std::runtime_error("invalid argument: " + arg);
    }
  }

  if (r.t == RunMode::local || r.t == RunMode::batchsend) {
    if (r.groups.size() == 0) {
      throw std::runtime_error("can't record data because no groups have been specified");
    }
    if (::access(r.groupServerDir.c_str(), W_OK) != 0) {
      throw std::runtime_error("can't record domain socket for producer registration (" + std::string(strerror(errno)) + "): " + r.groupServerDir);
    }
  }
  return r;
}

std::string instantiateDir(const std::string& groupName, const std::string& dir) {
  // instantiate group name
  std::string x = hobbes::str::replace<char>(dir, "$GROUP", groupName);

  // instantiate date
  time_t now = ::time(0);
  if (const tm* t = localtime(&now)) {
    std::ostringstream ss;
    ss << t->tm_year + 1900 << "." << (t->tm_mon < 10 ? "0" : "") << t->tm_mon + 1 << "." << (t->tm_mday < 10 ? "0" : "") << t->tm_mday;
    x = hobbes::str::replace<char>(x, "$DATE", ss.str());
  }

  // that's all we will substitute
  return x;
}

void evalGroupHostConnection(SessionGroup* sg, const std::string& groupName, const RunMode& m, std::vector<std::thread>* ts, int c) {
  try {
    uint8_t cmd=0;
    hobbes::fdread(c, (char*)&cmd, sizeof(cmd));
  
    uint64_t pid=0,tid=0;
    hobbes::fdread(c, (char*)&pid, sizeof(pid));
    hobbes::fdread(c, (char*)&tid, sizeof(tid));
    out << "queue registered for group '" << groupName << "' from " << pid << ":" << tid << std::endl;
  
    auto qc = hobbes::storage::consumeGroup(groupName, hobbes::storage::ProcThread(pid, tid));
  
    std::string d = instantiateDir(groupName, m.dir);
    switch (m.t) {
    case RunMode::local:
      ts->push_back(std::thread(std::bind(&recordLocalData, sg, qc, d)));
      break;
    case RunMode::batchsend:
      ts->push_back(std::thread(std::bind(&pushLocalData, qc, groupName, ensureDirExists(d), m.clevel, m.batchsendsize, m.batchsendtime, m.sendto)));
      break;
    default:
      break;
    }
  } catch (std::exception& ex) {
    out << "error on connection for '" << groupName << "': " << ex.what() << std::endl;
    hobbes::unregisterEventHandler(c);
    close(c);
  }
}

void runGroupHost(const std::string& groupName, const RunMode& m, std::vector<std::thread>* ts) {
  SessionGroup* sg = makeSessionGroup(m.consolidate);

  hobbes::registerEventHandler(
    hobbes::storage::makeGroupHost(groupName, m.groupServerDir),
    [sg,groupName,ts,&m](int s) {
      out << "new connection for '" << groupName << "'" << std::endl;
  
      int c = accept(s, 0, 0);
      if (c != -1) {
        try {
          uint32_t version = 0;
          hobbes::fdread(c, &version);
          if (version != HSTORE_VERSION) {
            out << "disconnected client for '" << groupName << "' due to version mismatch (expected " << HSTORE_VERSION << " but got " << version << ")" << std::endl;
            close(c);
          } else {
            hobbes::registerEventHandler(
              c,
              [sg,groupName,ts,&m](int c) {
                evalGroupHostConnection(sg, groupName, m, ts, c);
              }
            );
          }
        } catch (std::exception& ex) {
          out << "error on connection for '" << groupName << "': " << ex.what() << std::endl;
          close(c);
        }
      }
    }
  );
}

void run(const RunMode& m) {
  out << "hog running in mode : " << m << std::endl;
  if (m.t == RunMode::batchrecv) {
    pullRemoteDataT(m.dir, m.localport, m.consolidate).join();
  } else if (m.groups.size() > 0) {
    std::vector<std::thread> tasks;

    for (auto g : m.groups) {
      try {
        out << "install a monitor for the '" << g << "' group" << std::endl;
        runGroupHost(g, m, &tasks);
      } catch (std::exception& ex) {
        out << "error while installing a monitor for '" << g << "': " << ex.what() << std::endl;
        throw;
      }
    }
    hobbes::runEventLoop();
  }
}

}

int main(int argc, const char** argv) {
  try {
    hog::run(hog::config(argc, argv));
    return 0;
  } catch (std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return -1;
  }
}
