var bt = require('../lib/bluetooth-serial-port.js');
var Bt = new bt.BluetoothSerialPort();

console.log('Checking client...');

[
    'inquire', 'findSerialPortChannel', 'connect', 'write', 'on', 'close'
].forEach(function (fun) {
    if (typeof Bt[fun] !== 'function')
        throw new Error("Assert failed: " + fun +
            "should be a function but is " + (typeof Bt[fun]));
});

const channelFound = (channel) => {
    console.log(`Found channel at ${channel}`)
}

const channelNotFound = () => {
    console.log(`Channel not found`);
}

const found = (address, name, services) => {
    // if (!address && !name) console.log('Could not find')
    console.log(`\nFound ${name} at ${address} with following services:`);
    for (let service of services) {
        console.log(`${service}`);
    }
    Bt.findSerialPortChannel(address, channelFound, channelNotFound)
}

const finished = (devices) => {
    console.log('finished');
}

Bt.on('found', found)
Bt.on('finished', finished)
Bt.inquire();

console.log('Ok!');

if (process.platform === 'linux') {
    console.log('Checking server...');

    var ServerBt = new bt.BluetoothSerialPortServer();
    [
        'listen', 'write', 'on', 'close'
    ].forEach(function (fun) {
        if (typeof ServerBt[fun] !== 'function')
            throw new Error("Assert failed: " + fun +
                "should be a function but is " + (typeof ServerBt[fun]));
    });

    console.log('Ok!');
}

// process.exit(0);

