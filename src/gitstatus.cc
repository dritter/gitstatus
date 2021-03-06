// Copyright 2019 Roman Perepelitsa.
//
// This file is part of GitStatus.
//
// GitStatus is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// GitStatus is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with GitStatus. If not, see <https://www.gnu.org/licenses/>.

#include <cstddef>

#include <future>
#include <string>

#include <git2.h>

#include "check.h"
#include "git.h"
#include "logging.h"
#include "options.h"
#include "repo_cache.h"
#include "request.h"
#include "response.h"
#include "scope_guard.h"
#include "timer.h"

namespace gitstatus {
namespace {

using namespace std::string_literals;

void ProcessRequest(const Options& opts, RepoCache& cache, Request req) {
  Timer timer;
  ON_SCOPE_EXIT(&) { timer.Report("request"); };

  ResponseWriter resp(req.id);
  Repo* repo = cache.Open(req.dir);
  if (!repo) return;

  git_reference* head = Head(repo->repo());
  if (!head) return;
  ON_SCOPE_EXIT(=) { git_reference_free(head); };

  const git_oid* head_target = git_reference_target(head);
  std::future<std::string> tag = repo->GetTagName(head_target);
  ON_SCOPE_EXIT(&) {
    if (tag.valid()) {
      try {
        tag.wait();
      } catch (const Exception&) {
      }
    }
  };

  // Repository working directory.
  StringView workdir = git_repository_workdir(repo->repo()) ?: "";
  if (workdir.len == 0) return;
  if (workdir.len > 1 && workdir.ptr[workdir.len - 1] == '/') --workdir.len;
  resp.Print(workdir);

  // Revision. Either 40 hex digits or an empty string for empty repo.
  resp.Print(head_target ? git_oid_tostr_s(head_target) : "");

  // Local branch name (e.g., "master") or empty string if not on a branch.
  resp.Print(LocalBranchName(head));

  git_reference* upstream = Upstream(head);
  ON_SCOPE_EXIT(=) {
    if (upstream) git_reference_free(upstream);
  };
  // Upstream branch name.
  resp.Print(upstream ? RemoteBranchName(repo->repo(), upstream) : "");

  // Remote url.
  resp.Print(upstream ? RemoteUrl(repo->repo(), upstream) : "");

  // Repository state, A.K.A. action.
  resp.Print(RepoState(repo->repo()));

  const IndexStats stats = repo->GetIndexStats(head_target, opts.dirty_max_index_size);

  // 1 if there are staged changes, 0 otherwise.
  resp.Print(stats.has_staged);
  // 1 if there are unstaged changes, 0 if there aren't, -1 if we don't know.
  resp.Print(stats.has_unstaged);
  // 1 if there are untracked changes, 0 if there aren't, -1 if we don't know.
  resp.Print(stats.has_untracked);

  if (upstream) {
    // Number of commits we are ahead of upstream.
    resp.Print(CountRange(repo->repo(), git_reference_shorthand(upstream) + "..HEAD"s));
    // Number of commits we are behind upstream.
    resp.Print(CountRange(repo->repo(), "HEAD.."s + git_reference_shorthand(upstream)));
  } else {
    resp.Print(0);
    resp.Print(0);
  }

  // Number of stashes.
  resp.Print(NumStashes(repo->repo()));

  // Tag or empty string.
  resp.Print(tag.get());

  resp.Dump("with git status");
}

int GitStatus(int argc, char** argv) {
  for (int i = 0; i != argc; ++i) LOG(INFO) << "argv[" << i << "]: " << argv[i];

  Options opts = ParseOptions(argc, argv);
  RequestReader reader(fileno(stdin), opts.lock_fd, opts.sigwinch_pid);
  RepoCache cache;

  git_libgit2_opts(GIT_OPT_ENABLE_STRICT_HASH_VERIFICATION, 0);
  git_libgit2_opts(GIT_OPT_DISABLE_INDEX_CHECKSUM_VERIFICATION, 1);
  git_libgit2_opts(GIT_OPT_DISABLE_INDEX_FILEPATH_VALIDATION, 1);
  git_libgit2_init();
  InitThreadPool(opts.num_threads);

  while (true) {
    try {
      Request req = reader.ReadRequest();
      LOG(INFO) << "Processing request: " << req;
      try {
        ProcessRequest(opts, cache, req);
        LOG(INFO) << "Successfully processed request: " << req;
      } catch (const Exception&) {
        LOG(ERROR) << "Error processing request: " << req;
      }
    } catch (const Exception&) {
    }
  }
}

}  // namespace
}  // namespace gitstatus

int main(int argc, char** argv) { gitstatus::GitStatus(argc, argv); }
