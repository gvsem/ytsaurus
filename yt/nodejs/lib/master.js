var cluster = require("cluster");

var utils = require("./utils");

////////////////////////////////////////////////////////////////////////////////

var __DBG;
var __DIE;

__DBG = require("./debug").that("C", "Cluster Master");
__DIE = function dieOfTrue(condition, message)
{
    if (condition) {
        console.error("*** Aborting: " + message);
        process.abort();
    }
};

var TIMEOUT_INITIAL   = 5000;
var TIMEOUT_HEARTBEAT = 30000;
var TIMEOUT_COOLDOWN  = 60000;

var MEMORY_PRESSURE_LIMIT = 128 * 1024 * 1024;
var MEMORY_PRESSURE_HIT_COOLDOWN = 60000;
var MEMORY_PRESSURE_HIT_TIMESTAMP = 0;

////////////////////////////////////////////////////////////////////////////////

function YtClusterHandle(logger, logger_ts, profiler, worker)
{
    this.logger     = logger;
    this.logger_ts  = logger_ts;
    this.profiler   = profiler;
    this.worker     = worker;
    this.state      = "unknown";
    this.young      = true;
    this.alive      = true;
    this.created_at = new Date();
    this.updated_at = new Date();
    this.timeout_at = null;

    this.__cached_wid = undefined;
    this.__cached_pid = undefined;

    this.postponeDeath(TIMEOUT_INITIAL);
    // Initial startup should be fast; 5 seconds is enough.
}

YtClusterHandle.prototype.getWid = function()
{
    if (!this.__cached_wid) {
        this.__cached_wid = this.worker ? this.worker.id : -1;
    }
    return this.__cached_wid;
};

YtClusterHandle.prototype.getPid = function()
{
    if (!this.__cached_pid) {
        this.__cached_pid = this.worker ? this.worker.process.pid : -1;
    }
    return this.__cached_pid;
};

YtClusterHandle.prototype.toString = function()
{
    return require("util").format("<YtClusterHandle wid=%s pid=%s state=%s>",
        this.getWid(),
        this.getPid(),
        this.state);
};

YtClusterHandle.prototype.kill = function()
{
    if (!this.alive) {
        return;
    }

    this.worker.send({ type : "gracefullyDie" });
};

YtClusterHandle.prototype.destroy = function()
{
    if (!this.alive) {
        return;
    }

    if (this.timeout_at) {
        clearTimeout(this.timeout_at);
        this.timeout_at = null;
    }

    this.logger     = null;
    this.logger_ut  = null;
    this.worker     = null;
    this.state      = "destroyed";
    this.alive      = false;
    this.updated_at = new Date();
};

YtClusterHandle.prototype.handleMessage = function(message)
{
    if (!this.alive) {
        return; // We are not interested anymore.
    }

    if (!message || !message.type) {
        return; // Improper message format.
    }

    switch (message.type) {
        case "log":
            this.handleLog(message.level, message.message, message.payload);
            break;
        case "profile":
            this.handleProfile(message.method, message.metric, message.tags, message.value);
            break;
        case "heartbeat":
            if (!__DBG.$) {
                this.worker.send({ type : "heartbeat" });
            }
            this.postponeDeath(TIMEOUT_HEARTBEAT);
            break;
        case "alive":
            this.state = "alive";
            this.postponeDeath(TIMEOUT_COOLDOWN);
            break;
        case "stopping":
            this.state = "stopping";
            this.postponeDeath();
            break;
        case "stopped":
            this.state = "stopped";
            this.postponeDeath(TIMEOUT_COOLDOWN);
            break;
        default:
            this.logger_ts.warn(
                "Received unknown message of type '" + message.type +
                "' from worker " + this.toString());
            break;
    }
};

YtClusterHandle.prototype.handleLog = function(level, message, payload)
{
    if (process.memoryUsage().rss < MEMORY_PRESSURE_LIMIT) {
        this.logger[level](message, payload);
    } else {
        var time_now = +(new Date());
        var time_next = MEMORY_PRESSURE_HIT_TIMESTAMP + MEMORY_PRESSURE_HIT_COOLDOWN;

        if (time_now > time_next) {
            this.logger_ts.warn("Logging is disabled due to high memory pressure");

            MEMORY_PRESSURE_HIT_TIMESTAMP = time_now;
        }
    }
};

YtClusterHandle.prototype.handleProfile = function(method, metric, tags, value)
{
    this.profiler[method](metric, tags, value);
};

YtClusterHandle.prototype.postponeDeath = function(timeout)
{
    if (!this.alive) {
        return;
    }

    if (__DBG.$) {
        return;
    }

    if (this.timeout_at) {
        clearTimeout(this.timeout_at);
        this.timeout_at = null;
    }

    if (timeout) {
        this.updated_at = new Date();
        this.timeout_at = setTimeout(this.ageToDeath.bind(this), timeout);
    }
};

YtClusterHandle.prototype.ageToDeath = function()
{
    if (!this.alive) {
        return;
    }

    this.logger_ts.info("Worker is not responding", {
        wid : this.getWid(),
        pid : this.getPid(),
        handle : this.toString()
    });

    this.certifyDeath();
};

YtClusterHandle.prototype.certifyDeath = function()
{
    if (!this.alive) {
        return;
    }

    this.logger_ts.info("Worker is dead", {
        wid : this.getWid(),
        pid : this.getPid(),
        handle : this.toString()
    });

    try {
        this.worker.send("violentlyDie");
        this.worker.disconnect();
    } catch (err) {
    }

    this.worker.process.kill("SIGKILL");

    this.destroy();
};

////////////////////////////////////////////////////////////////////////////////

function YtClusterMaster(logger, profiler, number_of_workers, cluster_options)
{
    __DBG("New");

    if (__DBG.$) {
        [
            "fork", "online", "listening", "disconnect", "exit"
        ].forEach(function(event) {
            cluster.on(event, function(worker) {
                __DBG(event + ": " + worker.id);
            });
        });
    }

    var getTS = function getTS() { return new Date().toISOString(); };

    this.logger = logger;
    this.logger_ts = new utils.TaggedLogger(logger, { timestamp: getTS });

    this.profiler = profiler;

    __DBG("Expected number of workers is " + number_of_workers);

    this.workers_expected = number_of_workers;
    this.workers_handles = {};

    this.timeout_for_respawn = null;
    this.timeout_for_shutdown = null;

    var self = this;

    cluster.on("fork", function(worker) {
        self.workers_handles[worker.id].getPid();
        self.workers_handles[worker.id].getWid();
    });

    cluster.on("exit", function(worker, code, signal) {
        __DIE(
            !self.workers_handles.hasOwnProperty(worker.id),
            "Received |message| event from the dead worker");

        self.logger_ts.info("Worker has exited", {
            wid    : worker.id,
            pid    : worker.process.pid,
            code   : code,
            signal : signal
        });

        self.workers_handles[worker.id].certifyDeath();
        delete self.workers_handles[worker.id];

        self.scheduleRespawnWorkers();
    });

    cluster.setupMaster(cluster_options);
}

YtClusterMaster.prototype.kickstart = function()
{
    while (this.countWorkers()[0] < this.workers_expected) {
        this.spawnNewWorker();
    }
};

YtClusterMaster.prototype.debug = function()
{
    var  p;
    for (p in this.workers_handles) {
        if (this.workers_handles.hasOwnProperty(p)) {
            var handle = this.workers_handles[p];
            console.error(" -> " + handle.toString());
        }
    }
};

YtClusterMaster.prototype.countWorkers = function()
{
    var  p, n_total = 0, n_young = 0;
    for (p in this.workers_handles) {
        if (this.workers_handles.hasOwnProperty(p)) {
            ++n_total;
            if (this.workers_handles[p].young) {
                ++n_young;
            }
        }
    }
    return [ n_total, n_young ];
};

YtClusterMaster.prototype.spawnNewWorker = function()
{
    var worker = cluster.fork();
    var handle = this.workers_handles[worker.id] =
        new YtClusterHandle(this.logger, this.logger_ts, this.profiler, worker);

    worker.on("message", handle.handleMessage.bind(handle));
    this.logger_ts.info("Spawned young worker");
};

YtClusterMaster.prototype.killOldWorker = function()
{
    var  p, handle;
    for (p in this.workers_handles) {
        if (this.workers_handles.hasOwnProperty(p)) {
            handle = this.workers_handles[p];
            if (!handle.young) {
                handle.kill();
                this.logger_ts.info("Killed old worker", {
                    handle : handle.toString()
                });
            }
        }
    }
};

YtClusterMaster.prototype.respawnWorkers = function()
{
    var current  = this.countWorkers();
    var n_total  = current[0];
    var n_young  = current[1];
    var n_target = this.workers_expected;

    // This method should decide whether a new worker is required and whether
    // a subsequent trailing check is required.
    var will_spawn = false;
    var will_kill = false;
    var will_reschedule = false;

    if (n_young === 0) {
        if (n_target > 0) {
            this.logger_ts.info("Young generation is dead; resurrecting...");
            will_spawn = true;
            will_reschedule = true;
        }
    } else {
        if (n_young < n_total) {
            this.logger_ts.info("Old generation is alive; killing...");
            will_kill = true;
            will_reschedule = false;
        }
        if (n_young < n_target) {
            this.logger_ts.info("More young workers required; spawning...");
            will_spawn = true;
            will_reschedule = true;
        }
    }

    if (will_spawn) {
        this.spawnNewWorker();
    }

    if (will_kill) {
        this.killOldWorker();
    }

    if (will_reschedule) {
        this.scheduleRespawnWorkers();
    }
};

YtClusterMaster.prototype.scheduleRespawnWorkers = function()
{
    if (this.timeout_for_respawn) {
        return;
    }

    var self = this;
    self.timeout_for_respawn = setTimeout(function() {
        self.timeout_for_respawn = null;
        self.respawnWorkers();
    }, 1000);
};

YtClusterMaster.prototype.restartWorkers = function()
{
    this.logger_ts.info("Starting rolling restart of workers");
    for (var i in this.workers_handles) {
        if (this.workers_handles.hasOwnProperty(i)) {
            this.workers_handles[i].young = false;
        }
    }
    this.scheduleRespawnWorkers();
};

YtClusterMaster.prototype.shutdownWorkers = function()
{
    // NB: Rely an actual cluster state, not on |this.workers_handles|.
    this.logger_ts.info("Starting graceful shutdown");
    var i;
    for (i in cluster.workers) {
        if (cluster.workers.hasOwnProperty(i)) {
            cluster.workers[i].send({ type : "gracefullyDie" });
        }
    }
    for (i in this.workers_handles) {
        if (this.workers_handles.hasOwnProperty(i)) {
            this.workers_handles[i].young = false;
        }
    }
    this.workers_expected = 0;
    setTimeout(this.shutdownWorkersLoop.bind(this), 1000);
};

YtClusterMaster.prototype.shutdownWorkersLoop = function()
{
    // NB: Rely an actual cluster state, not on |this.workers_handles|.
    var n = Object.keys(cluster.workers).length;
    if (n > 0) {
        this.logger_ts.info("There are " + n + " workers alive", { n : n });
        setTimeout(this.shutdownWorkersLoop.bind(this), 1000);
    } else {
        this.logger_ts.info("All workers gone");
        process.exit();
    }
};

exports.that = YtClusterMaster;

