const { fork } = require('child_process');
const nbind = require('nbind');
const path = require('path');

const attachBindings = require('./bindings');

// in electron renderer thread we have native webworkers,
// otherwise we need a module

let binding;
try {
  binding = nbind.init(path.join(__dirname, 'loot'));
} catch (err) {
  binding = nbind.init();
}

attachBindings(binding);

const lib = binding.lib;

class LootAsync {
  static create(gameId, gamePath, gameLocalPath, language, logCallback, callback) {
    const res = new LootAsync(gameId, gamePath, gameLocalPath, language, logCallback, (err) => {
      if (err !== null) {
        callback(err);
      } else {
        callback(null, res);
      }
    });
  }

  constructor(gameId, gamePath, gameLocalPath, language, logCallback, callback) {
    this.queue = [];
    this.logCallback = logCallback;

    this.currentCallback = () => {
      this.enqueue({
        type: 'init',
        args: [
          gameId,
          gamePath,
          gameLocalPath,
          language,
        ]
      }, callback);
    }

    this.makeProxy('updateMasterlist');
    this.makeProxy('getMasterlistRevision');
    this.makeProxy('loadLists');
    this.makeProxy('loadPlugins');
    this.makeProxy('getPlugin');
    this.makeProxy('getPluginMetadata');
    this.makeProxy('sortPlugins');
    this.makeProxy('setLoadOrder');
    this.makeProxy('getLoadOrder');
    this.makeProxy('loadCurrentLoadOrderState');
    this.makeProxy('isPluginActive');
    this.makeProxy('getGroups');
    this.makeProxy('getUserGroups');
    this.makeProxy('setUserGroups');

    this.worker = fork(`${__dirname}${path.sep}async.js`);
    this.worker.on('message', (...args) => this.handleResponse(...args));
  }

  makeProxy(name) {
    this[name] = (...args) => {
      this.enqueue({
        type: name,
        args: args.slice(0, args.length - 1),
      }, args[args.length - 1]);
    };
  }

  enqueue(message, callback) {
    if (this.currentCallback === null) {
      this.deliver(message, callback);
    } else {
      this.queue.push({ message, callback });
    }
  }

  deliver(message, callback) {
    this.currentCallback = callback;
    try {
      this.worker.send(message);
    } catch (err) {
      this.currentCallback(new Error('LOOT closed? Please check your log. Error was: ' + err.message));
      this.processQueue();
    }
  }

  processQueue() {
    if (this.queue.length > 0) {
      const next = this.queue.shift();
      this.deliver(next.message, next.callback);
    } else {
      this.currentCallback = null;
    }
  }

  handleResponse(msg) {
    // don't touch the queue when relaying logs
    if (msg.log) {
      this.logCallback(msg.log.level, msg.log.message);
      return;
    }

    // relay result, then process next request in the queue, if any
    try {
      if (msg.error) {
        this.currentCallback(new Error(msg.error));
      } else {
        this.currentCallback(null, msg.result);
      }
      this.processQueue();
    } catch (err) {
      // don't want to suppress an error but
      // if we don't trigger the queue here, this proxy is dead
      this.processQueue();
      throw err;
    }
  }
}

module.exports = Object.assign(lib, {
  LootAsync
});
