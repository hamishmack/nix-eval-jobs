// doesn't exist on macOS
// IWYU pragma: no_include <bits/types/struct_rusage.h>

#include <nix/expr/eval-error.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/terminal.hh>
#include <nix/expr/attr-path.hh>
#include <nix/store/local-fs-store.hh>
#include <nix/store/globals.hh>
#include <nix/cmd/installable-flake.hh>
#include <nix/expr/value-to-json.hh>
#include <sys/resource.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <iostream>
// NOLINTBEGIN(modernize-deprecated-headers)
// misc-include-cleaner wants this header rather than the C++ version
#include <stdlib.h>
// NOLINTEND(modernize-deprecated-headers)
#include <exception>
#include <filesystem>
#include <functional>
#include <nix/expr/attr-set.hh>
#include <nix/cmd/common-eval-args.hh>
#include <nix/util/error.hh>
#include <nix/expr/eval.hh>
#include <nix/util/file-system.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/flake.hh>
#include <nix/expr/get-drvs.hh>
#include <nix/util/logging.hh>
#include <nix/store/outputs-spec.hh>
#include <nix/util/ref.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/util/types.hh>
#include <nix/util/util.hh>
#include <nix/expr/value.hh>
#include <nix/expr/value/context.hh>
#include <nlohmann/json_fwd.hpp>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <v3/eval_jobs_api.hh>
#include <v3/errors.hh> // nix::v3::CallDepthError (fatal mapping, review A4)
#include <v3/primop.hh> // nix::v3::setFlakeSettings (wire flakeSettings for getFlake)

#include "worker.hh"
#include "drv.hh"
#include "response.hh"
#include "buffered-io.hh"
#include "eval-args.hh"
#include "store.hh"

namespace nix {
struct Expr;
} // namespace nix

namespace {
auto releaseExprTopLevelValue(nix::EvalState &state, nix::Bindings &autoArgs,
                              MyArgs &args) -> nix::Value * {
    nix::Value vTop;

    if (args.fromArgs) {
        nix::Expr *expr =
            state.parseExprFromString(args.releaseExpr, state.rootPath("."));
        state.eval(expr, vTop);
    } else {
        state.evalFile(lookupFileArg(state, args.releaseExpr), vTop);
    }

    auto *vRoot = state.allocValue();

    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

auto evaluateFlake(const nix::ref<nix::EvalState> &state,
                   const std::string &releaseExpr,
                   const nix::flake::LockFlags &lockFlags) -> nix::Value * {
    auto [flakeRef, fragment, outputSpec] =
        nix::parseFlakeRefWithFragmentAndExtendedOutputsSpec(
            nix::fetchSettings, releaseExpr,
            nix::absPath(std::filesystem::path(".")));

    nix::InstallableFlake flake{{},       state,      std::move(flakeRef),
                                fragment, outputSpec, {},
                                {},       lockFlags};

    // If no fragment specified, use callFlake to get the full flake structure
    // (just like :lf in the REPL)
    if (fragment.empty()) {
        auto *value = state->allocValue();
        nix::flake::callFlake(*state, *flake.getLockedFlake(), *value);
        return value;
    }
    // Fragment specified, use normal evaluation
    return flake.toValue(*state).first;
}

// Model B (NIX_EVAL_JOBS_PATCH_2026-07-18): build the v3 flake root handle.
//
// The tree-walker locks the flake and evaluates callFlake/toValue on a
// nix::Value; here we lock the same flake (a store/fetch leaf — the locking is
// NOT evaluation, so it stays on TW) to obtain the LOCKED, rev-pinned flakeref,
// then hand that ref to v3's native `getFlake` so v3 owns the heavy descent.
// The locked ref is load-bearing: using anything but the exact ref the TW path
// locked would diverge the drvPaths.
auto evaluateFlakeV3(const nix::ref<nix::EvalState> &state,
                     const std::string &releaseExpr,
                     const nix::flake::LockFlags &lockFlags,
                     const std::string &selectExpr)
    -> nix::v3::EvalJobsHandlePtr {
    auto [flakeRef, fragment, outputSpec] =
        nix::parseFlakeRefWithFragmentAndExtendedOutputsSpec(
            nix::fetchSettings, releaseExpr,
            nix::absPath(std::filesystem::path(".")));

    nix::InstallableFlake flake{{},       state,      std::move(flakeRef),
                                fragment, outputSpec, {},
                                {},       lockFlags};

    const std::string lockedRef =
        flake.getLockedFlake()->flake.lockedRef.to_string();

    return nix::v3::evalFlakeRoot(*state, lockedRef, fragment, selectExpr);
}

auto attrPathJoin(nlohmann::json input) -> std::string {
    return std::accumulate(
        input.begin(), input.end(), std::string(),
        [](const std::string &acc, std::string str) -> std::basic_string<char> {
            // Escape token if containing dots
            if (str.find('.') != std::string::npos) {
                str = "\"" + str + "\"";
            }
            return acc.empty() ? str : acc + "." + str;
        });
}

auto extractConstituents(nix::EvalState &state, nix::Value *value,
                         const MyArgs &args) -> Constituents {
    if (!args.constituents) {
        return {};
    }

    std::vector<std::string> constituents;
    std::vector<std::string> namedConstituents;
    bool globConstituents = false;

    const auto *aggregateAttr =
        value->attrs()->get(state.symbols.create("_hydraAggregate"));

    if (aggregateAttr != nullptr &&
        state.forceBool(*aggregateAttr->value, aggregateAttr->pos,
                        "while evaluating the `_hydraAggregate` attribute")) {

        const auto *constituentsAttr =
            value->attrs()->get(state.symbols.create("constituents"));

        if (constituentsAttr == nullptr) {
            state
                .error<nix::EvalError>(
                    "derivation must have a 'constituents' attribute")
                .debugThrow();
        }

        // Extract constituent paths from context
        nix::NixStringContext context;
        state.coerceToString(
            constituentsAttr->pos, *constituentsAttr->value, context,
            "while evaluating the `constituents` attribute", true, false);

        for (const auto &ctx : context) {
            std::visit(
                nix::overloaded{
                    [&](const nix::NixStringContextElem::Built &built) -> void {
                        constituents.push_back(
                            built.drvPath->to_string(*state.store));
                    },
                    [&](const nix::NixStringContextElem::Opaque &opaque
                        [[maybe_unused]]) -> void {},
                    [&](const nix::NixStringContextElem::DrvDeep &drvDeep
                        [[maybe_unused]]) -> void {},
                },
                ctx.raw);
        }

        // Extract named constituents
        state.forceList(*constituentsAttr->value, constituentsAttr->pos,
                        "while evaluating the `constituents` attribute");
        auto constituentsList = constituentsAttr->value->listView();

        for (const auto &val : constituentsList) {
            state.forceValue(*val, nix::noPos);
            if (val->type() == nix::nString) {
                namedConstituents.emplace_back(val->c_str());
            }
        }

        // Check for glob constituents
        const auto *glob =
            value->attrs()->get(state.symbols.create("_hydraGlobConstituents"));
        globConstituents =
            glob != nullptr &&
            state.forceBool(
                *glob->value, glob->pos,
                "while evaluating the `_hydraGlobConstituents` attribute");
    }

    return Constituents{
        .constituents = std::move(constituents),
        .namedConstituents = std::move(namedConstituents),
        .globConstituents = globConstituents,
    };
}

auto applyExprToValue(nix::EvalState &state, nix::Value *value,
                      const std::string &applyExpr) -> nlohmann::json {
    if (applyExpr.empty()) {
        return nlohmann::json{};
    }

    auto *expr = state.parseExprFromString(applyExpr, state.rootPath("."));

    nix::Value vApply;
    nix::Value vRes;

    state.eval(expr, vApply);
    state.callFunction(vApply, *value, vRes, nix::noPos);
    state.forceAttrs(vRes, nix::noPos, "apply needs to evaluate to an attrset");

    nix::NixStringContext context;
    std::stringstream stream;
    nix::printValueAsJSON(state, true, vRes, nix::noPos, stream, context);

    return nlohmann::json::parse(stream.str());
}

auto registerGCRoot(nix::EvalState &state, const Drv &drv, const MyArgs &args)
    -> void {
    if (args.gcRootsDir.empty() || nix::settings.readOnlyMode) {
        return;
    }

    const std::filesystem::path root =
        args.gcRootsDir / std::string(drv.drvPath.to_string());

    if (!nix::pathExists(root)) {
        auto localStore = state.store.dynamic_pointer_cast<nix::LocalFSStore>();
        if (localStore) {
            localStore->addPermRoot(drv.drvPath, root);
        }
        // If not a local store, we can't create GC roots
    }
}

auto collectAttrsForRecursion(nix::EvalState &state, nix::Value *value,
                              const nlohmann::json &path, const MyArgs &args)
    -> std::vector<std::string> {
    std::vector<std::string> attrs;
    bool recurse =
        args.forceRecurse ||
        path.empty(); // Don't require recurseForDerivations for top-level

    for (auto &attr : value->attrs()->lexicographicOrder(state.symbols)) {
        const std::string_view &name = state.symbols[attr->name];
        attrs.emplace_back(name);

        if (!args.forceRecurse && name == "recurseForDerivations") {
            const auto *attrv =
                value->attrs()->get(nix::EvalState::s.recurseForDerivations);
            recurse = state.forceBool(*attrv->value, attrv->pos,
                                      "while evaluating recurseForDerivations");
        }
    }

    return recurse ? attrs : std::vector<std::string>{};
}

auto processDerivation(nix::EvalState &state, nix::Value *value,
                       std::string &attrPathS, const nlohmann::json &path,
                       MyArgs &args) -> Response::Payload {
    auto packageInfo = nix::getDerivation(state, *value, false);
    if (!packageInfo) {
        auto attrs = collectAttrsForRecursion(state, value, path, args);
        return Response::Attrs{std::move(attrs)};
    }

    // Extract constituents if enabled
    auto constituents = extractConstituents(state, value, args);

    // Apply expression if provided
    std::optional<nlohmann::json> extraValue;
    if (!args.applyExpr.empty()) {
        extraValue = applyExprToValue(state, value, args.applyExpr);
    }

    // Create derivation info
    auto drv = Drv::fromPackageInfo(attrPathS, state, *packageInfo, args,
                                    std::move(constituents));

    // Register GC root
    registerGCRoot(state, drv, args);

    return Response::Job{std::move(drv), std::move(extraValue)};
}

// Model B: the v3-native counterpart of processDerivation.  The current job
// value already lives in the handle's GC-rooted slot (set by descendAttrPath).
// Mirrors processDerivation + collectAttrsForRecursion, but reads the value
// via the v3 accessors.
auto processDerivationV3(nix::EvalState &state, nix::v3::EvalJobsHandle &v3h,
                         std::string &attrPathS, const nlohmann::json &path,
                         MyArgs &args) -> Response::Payload {
    if (!nix::v3::isDerivation(v3h)) {
        // Not a derivation → discover children to recurse into
        // (collectAttrsForRecursion's value-side logic).  forceRecurse is
        // passed THROUGH: the accessor then skips reading
        // `recurseForDerivations` entirely (the tree-walker's
        // `!args.forceRecurse &&` guard) and hard-errors on a non-bool
        // otherwise — exact TW parity (review F3).
        bool recurse = args.forceRecurse || path.empty();
        auto attrs = nix::v3::childAttrNames(v3h, recurse, args.forceRecurse);
        return Response::Attrs{recurse ? std::move(attrs)
                                       : std::vector<std::string>{}};
    }

    // NOTE: aggregate jobs (`_hydraAggregate`) under --constituents never
    // reach this function — processJobRequest routes them through the
    // original tree-walker path (jobNeedsTreeWalker, review F2), because
    // constituent extraction needs coerceToString-with-context on a TW
    // Value.  --apply is likewise TW-only (hydra does not use it); a v3 job
    // carries no extraValue.  Core discovery, detection, and extraction are
    // fully v3-native.
    Constituents constituents{};
    std::optional<nlohmann::json> extraValue;

    auto drv = Drv::fromV3(attrPathS, state, v3h, args, std::move(constituents));

    registerGCRoot(state, drv, args);

    return Response::Job{std::move(drv), std::move(extraValue)};
}

auto initializeRootValue(const nix::ref<nix::EvalState> &state,
                         nix::Bindings &autoArgs, MyArgs &args)
    -> nix::Value * {
    nix::Value *vEvaluated =
        args.flake ? evaluateFlake(state, args.releaseExpr, args.lockFlags)
                   : releaseExprTopLevelValue(*state, autoArgs, args);

    if (args.selectExpr.empty()) {
        return vEvaluated;
    }

    // Apply the provided select function
    auto *selectExpr =
        state->parseExprFromString(args.selectExpr, state->rootPath("."));

    nix::Value vSelect;
    state->eval(selectExpr, vSelect);

    nix::Value *vSelected = state->allocValue();
    state->callFunction(vSelect, *vEvaluated, *vSelected, nix::noPos);
    state->forceAttrs(
        *vSelected, nix::noPos,
        "'--select' must evaluate to an attrset (the traversal root)");

    return vSelected;
}

auto shouldRestart(const MyArgs &args) -> bool {
    struct rusage resourceUsage = {}; // NOLINT(misc-include-cleaner)
    getrusage(RUSAGE_SELF, &resourceUsage);
    const size_t maxrss =
        resourceUsage
            .ru_maxrss; // NOLINT(cppcoreguidelines-pro-type-union-access)
    static constexpr size_t KB_TO_BYTES = 1024;
    return maxrss > args.maxMemorySize * KB_TO_BYTES;
}

auto processJobRequest(nix::EvalState &state, LineReader &fromReader,
                       nix::AutoCloseFD &toParent, nix::Bindings &autoArgs,
                       const std::function<nix::Value *()> &ensureTwRoot,
                       nix::v3::EvalJobsHandlePtr &v3Handle,
                       const std::function<void()> &rebuildV3, MyArgs &args)
    -> bool {
    /* Wait for the collector to send us a job name. */
    if (tryWriteLine(toParent.get(), "next") < 0) {
        return false; // main process died
    }

    auto line = fromReader.readLine();
    if (line == "exit") {
        return false;
    }

    if (!nix::hasPrefix(line, "do ")) {
        std::cerr << "worker error: received invalid command '" << line
                  << "'\n";
        abort();
    }

    auto path = nlohmann::json::parse(line.substr(3));
    auto attrPathS = attrPathJoin(path);

    /* Evaluate it and send info back to the collector. */
    Response::Payload payload = [&]() -> Response::Payload {
        try {
            // Model B: when a v3 handle is present (NIX_V3_DIRECT_EVAL),
            // v3 owns the descent/detection/extraction — EXCEPT for the
            // three job shapes jobNeedsTreeWalker flags (aggregate jobs
            // under --constituents, functor attrsets, lambda values); those
            // fall through to the ORIGINAL tree-walker block below for
            // exact parity (review F2+F4), against a lazily-created TW root.
            //
            // Error hardening (review, 2026-07-20): any exception escaping
            // the v3 section (an eval error in a job is NORMAL in Hydra
            // jobsets) unwinds mid-dispatch and may leave the handle's
            // VMState frames/stacks in an indeterminate state.  Rather than
            // reason about unwind-safety of the VM internals, we REBUILD the
            // handle (cheap: root re-force is warm via the disk/applied
            // caches) and rethrow so the outer catch still emits the same
            // per-job Error response the tree-walker would.
            if (v3Handle) {
                try {
                    nix::v3::descendAttrPath(
                        *v3Handle, path.get<std::vector<std::string>>());
                    if (!nix::v3::jobNeedsTreeWalker(*v3Handle,
                                                     args.constituents)) {
                        if (nix::v3::isAttrs(*v3Handle)) {
                            return processDerivationV3(state, *v3Handle,
                                                       attrPathS, path, args);
                        }
                        // We ignore everything that cannot be built
                        return Response::Attrs{{}};
                    }
                    // else: parity fallback → tree-walker path below.
                } catch (...) {
                    rebuildV3(); // fresh VMState for the NEXT job
                    throw;       // outer catch → per-job Error response
                }
            }

            auto *vTmp =
                nix::findAlongAttrPath(state, attrPathS, autoArgs,
                                       *ensureTwRoot())
                    .first;

            auto *value = state.allocValue();
            state.autoCallFunction(autoArgs, *vTmp, *value);

            if (value->type() == nix::nAttrs) {
                return processDerivation(state, value, attrPathS, path, args);
            }
            // We ignore everything that cannot be built
            return Response::Attrs{{}};
        } catch (nix::EvalError &e) {
            const auto &err = e.info();
            std::ostringstream oss;
            nix::showErrorInfo(oss, err, nix::loggerSettings.showTrace.get());
            auto msg = oss.str();

            // Print to STDERR for Hydra UI
            std::cerr << msg << "\n";
            return Response::Error{nix::filterANSIEscapes(msg, true)};
        } catch (const nix::v3::CallDepthError &e) {
            // Review A4 (2026-07-20): v3's kMaxCallDepth guard is the
            // analogue of the tree-walker's nix::StackOverflowError — mark
            // it FATAL so the collector aborts the eval exactly like the
            // stock tree-walker path does for infinite recursion.
            const auto *msg = e.what();
            std::cerr << msg << '\n';
            return Response::Error{
                .error = nix::filterANSIEscapes(msg, true),
                .fatal = true,
            };
        } catch (const std::exception &e) {
            // FIXME: for some reason the catch block above doesn't trigger on
            // macOS (?)
            const auto *msg = e.what();
            std::cerr << msg << '\n';
            return Response::Error{
                .error = nix::filterANSIEscapes(msg, true),
                // Nix 2.34 throws `StackOverflowError` whreas before, Nix
                // actually exhausted the C/C++ stack and crashed the worker.
                //
                // Mark this error fatal so the collector replicates the old
                // fail-on-infinite-recursion behavior.
                .fatal = dynamic_cast<const nix::StackOverflowError *>(&e) !=
                         nullptr,
            };
        }
    }();

    Response const response{
        .attr = attrPathS,
        .attrPath = path.get<std::vector<std::string>>(),
        .payload = std::move(payload),
    };
    nlohmann::json const reply = response;
    if (tryWriteLine(toParent.get(), reply.dump()) < 0) {
        return false; // main process died
    }

    /* Check if we should restart due to memory usage */
    return !shouldRestart(args);
}

} // namespace

void worker(
    MyArgs &args,
    nix::AutoCloseFD &toParent, // NOLINT(bugprone-easily-swappable-parameters)
    nix::AutoCloseFD &fromParent) {

    auto evalStore = nix_eval_jobs::openStore(args.evalStoreUrl);
    auto state = nix::make_ref<nix::EvalState>(
        args.lookupPath, evalStore, nix::fetchSettings, nix::evalSettings);
    nix::Bindings &autoArgs = *args.getAutoArgs(*state);

    // Model B gate (mirrors the `nix` CLI's semantics):
    //   NIX_V3_DIRECT_EVAL — route flake jobsets through the v3 bytecode VM;
    //     unset falls back to the tree-walker (byte-for-byte the original path).
    //   NIX_V3_REQUIRE     — hard-fail if v3 does not engage (no silent TW
    //     fallback), so CI can prove the v3 path is exercised.
    const bool v3Direct = std::getenv("NIX_V3_DIRECT_EVAL") != nullptr;
    const bool v3Require = std::getenv("NIX_V3_REQUIRE") != nullptr;

    nix::v3::EvalJobsHandlePtr v3Handle; // null unless v3 engages
    nix::Value *vRoot = nullptr;

    // Review F6 (2026-07-20): v3's native getFlake re-locks the (rev-pinned)
    // flakeref internally with DEFAULT lock flags — it cannot honor
    // --override-input / --update-input / --no-write-lock-file-style
    // overrides or an external --reference-lock-file, so engaging v3 with
    // any of those set would silently evaluate the UN-overridden lock.
    // Hydra passes none of them; fall back to the tree-walker when present.
    const bool lockFlagsV3Compatible =
        args.lockFlags.inputOverrides.empty() &&
        args.lockFlags.inputUpdates.empty() &&
        !args.lockFlags.recreateLockFile &&
        !args.lockFlags.referenceLockFilePath.has_value();

    if (v3Direct && args.flake && !lockFlagsV3Compatible) {
        std::cerr << "nix-eval-jobs: v3-direct not engaged (lock-file "
                     "overrides --override-input/--update-input/"
                     "--reference-lock-file are tree-walker-only)\n";
    }

    // TWO v3-eligible invocation shapes (review 2026-07-20):
    //   (a) --flake ref[#fragment]           — the CLI/manual shape;
    //   (b) --expr '<expr>' with EMPTY autoArgs — THE shape Hydra actually
    //       uses (hydra-eval-jobset synthesizes `let flake =
    //       builtins.getFlake (toString "<locked-url>"); in flake.hydraJobs
    //       or flake.checks or (throw …)` and passes it via --expr; it
    //       never passes --flake).  Gating only on args.flake made the
    //       production deploy a silent no-op.
    // Legacy jobsets (a FILE positional and/or --arg/--argstr) never engage:
    // fromArgs is false and/or autoArgs is non-empty.
    const bool v3ExprShape = args.fromArgs && autoArgs.size() == 0;

    if (v3Direct && ((args.flake && lockFlagsV3Compatible) || v3ExprShape)) {
        // Wire libcmd's flakeSettings global into v3's thread-local pointer so
        // the native builtins.getFlake can lock/read the flake (mirrors the
        // `nix` CLI's main.cc startup wiring; the worker runs in its own
        // forked process/thread, so this thread_local set is per-worker).
        nix::v3::setFlakeSettings(&nix::flakeSettings);
        try {
            if (args.flake) {
                v3Handle = evaluateFlakeV3(state, args.releaseExpr,
                                           args.lockFlags, args.selectExpr);
                std::cerr << "nix-eval-jobs: v3-direct engaged (flake root "
                             "via builtins.getFlake)\n";
            } else {
                v3Handle = nix::v3::evalExprRoot(*state, args.releaseExpr,
                                                 args.selectExpr);
                std::cerr << "nix-eval-jobs: v3-direct engaged (--expr root "
                             "via the v3 pipeline)\n";
            }
        } catch (const std::exception &e) {
            if (v3Require) {
                throw; // no silent fallback under NIX_V3_REQUIRE
            }
            std::cerr << "nix-eval-jobs: v3-direct root eval failed, falling "
                         "back to the tree-walker: "
                      << e.what() << '\n';
            v3Handle.reset();
        }
    }

    if (v3Require && !v3Handle) {
        throw nix::Error(
            "NIX_V3_REQUIRE=1 but v3-direct did not engage (supported: "
            "--flake without lock-file overrides, or --expr without "
            "--arg/--argstr)");
    }

    if (!v3Handle) {
        vRoot = initializeRootValue(state, autoArgs, args);
    }

    // Lazy tree-walker root for the v3 mode's parity-fallback jobs
    // (aggregate/functor/lambda — jobNeedsTreeWalker).  Evaluated at most
    // once per worker, and only if such a job actually occurs.
    auto ensureTwRoot = [&]() -> nix::Value * {
        if (vRoot == nullptr) {
            std::cerr << "nix-eval-jobs: initializing tree-walker root for a "
                         "parity-fallback job (aggregate/functor/lambda)\n";
            vRoot = initializeRootValue(state, autoArgs, args);
        }
        return vRoot;
    };

    // Rebuild the v3 handle after a per-job v3 exception (see
    // processJobRequest).  Order is load-bearing: reset() FIRST — the
    // handle's two GcRoot registrations are popped LIFO while they are the
    // stack's top entries (GcRoot::~GcRoot pops blindly); constructing the
    // replacement first would interleave push/pop and corrupt the root
    // stack.  If the rebuild itself fails, remaining jobs on this worker
    // use the tree-walker root (exactly the engagement-failure fallback).
    auto rebuildV3 = [&]() noexcept {
        try {
            v3Handle.reset();
            v3Handle = args.flake
                           ? evaluateFlakeV3(state, args.releaseExpr,
                                             args.lockFlags, args.selectExpr)
                           : nix::v3::evalExprRoot(*state, args.releaseExpr,
                                                   args.selectExpr);
        } catch (...) {
            v3Handle.reset();
            std::cerr << "nix-eval-jobs: v3 handle rebuild failed; remaining "
                         "jobs on this worker use the tree-walker\n";
        }
    };

    LineReader fromReader(fromParent.release());

    while (processJobRequest(*state, fromReader, toParent, autoArgs,
                             ensureTwRoot, v3Handle, rebuildV3, args)) {
        // Continue processing jobs until we need to exit
    }

    if (tryWriteLine(toParent.get(), "restart") < 0) {
        return; // main process died
    };
}
