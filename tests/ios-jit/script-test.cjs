// SPDX-License-Identifier: GPL-2.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname,
    '../../src/app/Resources/jit.js'), 'utf8');
const le = value => BigInt(value).toString(16).padStart(16, '0').match(/../g).reverse().join('');
const brk = immediate => ((0xd4200000 | (immediate << 5)) >>> 0)
    .toString(16).match(/../g).reverse().join('');
const stop = (command, address = 0n, bytes = 0n) =>
    `T05thread:1;20:${le(0x100000100n)};10:${le(command)};00:${le(address)};01:${le(bytes)};`;

function run(stops, options = {}) {
    const commands = [], logs = [];
    let resumes = 0;
    const context = vm.createContext({
        get_pid: () => 42,
        log: message => logs.push(message),
        prepare_memory_region: () => { throw Error('Truncating framework path used'); },
        send_command: command => {
            commands.push(command);
            if (command.startsWith('vAttach;')) return options.attach || stop(0n);
            if (command === 'c') {
                resumes++;
                if (!stops.length) throw Error('Unexpected additional resume');
                return stops.shift();
            }
            if (command[0] === 'm') return options.instruction || brk(0xf00d);
            if (command[0] === 'M') return options.writeReply?.(command) ?? 'OK';
            if (command.startsWith('_M')) return options.allocation || '7000000000';
            if (command.startsWith('P20=')) return options.pcReply || 'OK';
            return 'OK';
        },
    });
    let error;
    try { vm.runInContext(source, context, {timeout: 1000}); } catch (e) { error = e; }
    return {commands, logs, resumes, error, context};
}

let cases = 0;
function check(name, test) { test(); cases++; console.log(`PASS ${name}`); }
check('high address page writes and return register retain all bits', () => {
    const result = run([stop(1n, 0x7000000000n, 32768n), stop(0n)]);
    assert.equal(result.error, undefined);
    assert.deepEqual(result.commands.filter(c => c[0] === 'M'),
        ['M7000000000,1:69', 'M7000004000,1:69']);
    assert.ok(result.commands.includes('P0=0000000070000000;thread:1;'));
    assert.equal(result.commands.at(-1), 'D');
});
check('page writes cross the 36-bit and 40-bit boundaries', () => {
    for (const [start, next] of [['fffffc000', '1000000000'], ['ffffffc000', '10000000000']]) {
        const result = run([stop(1n, BigInt(`0x${start}`), 32768n), stop(0n)]);
        assert.equal(result.error, undefined);
        assert.deepEqual(result.commands.filter(c => c[0] === 'M'),
            [`M${start},1:69`, `M${next},1:69`]);
    }
});
check('allocate path also validates every page', () => {
    const result = run([stop(1n, 0n, 16384n), stop(0n)]);
    assert.equal(result.error, undefined);
    assert.ok(result.commands.includes('_M4000,rx'));
    assert.ok(result.commands.includes('M7000000000,1:69'));
});
check('page write failure returns null and detaches immediately', () => {
    const result = run([stop(1n, 0x7000000000n, 49152n)], {
        writeReply: c => c === 'M7000004000,1:69' ? 'E08' : 'OK',
    });
    assert.match(result.error.message, /M7000004000,1:69: E08/);
    assert.ok(result.commands.includes('P0=0000000000000000;thread:1;'));
    assert.equal(result.commands.filter(c => c[0] === 'M').length, 2);
    assert.equal(result.commands.at(-1), 'D');
});
check('execution fault is reported after one stop', () => {
    const result = run([stop(0n).replace('T05', 'T0b')]);
    assert.match(result.error.message, /Unexpected stop/);
    assert.equal(result.resumes, 1);
    assert.equal(result.commands.at(-1), 'D');
});
check('non-BRK instruction with a matching immediate is rejected', () => {
    const result = run([stop(0n)], {instruction: '200d8052'});
    assert.match(result.error.message, /Unexpected instruction/);
    assert.ok(!result.commands.some(c => c.startsWith('P20=')));
});
check('malformed stop and unknown command terminate', () => {
    for (const reply of ['T05thread:1;', stop(99n)]) {
        const result = run([reply]);
        assert.ok(result.error);
        assert.equal(result.resumes, 1);
        assert.equal(result.commands.at(-1), 'D');
    }
});
check('register write and allocation errors propagate', () => {
    for (const options of [{pcReply: 'E01'}, {allocation: 'E08'}]) {
        const result = run([stop(1n, 0n, 16384n)], options);
        assert.ok(result.error);
        assert.equal(result.commands.at(-1), 'D');
    }
});
check('unaligned and overflowing ranges perform no page writes', () => {
    for (const [address, bytes] of [[1n,16384n], [0x7000000000n,1n],
        [0xffffffffffffc000n,32768n]]) {
        const result = run([stop(1n,address,bytes)]);
        assert.match(result.error.message, /Invalid JIT range/);
        assert.ok(!result.commands.some(c => c[0] === 'M'));
    }
});
check('register encoding preserves all 64 bits', () => {
    const result = run([stop(0n)]);
    assert.equal(vm.runInContext("littleEndianHexStringToNumber('efcdab8967452301')", result.context),
        0x0123456789abcdefn);
    assert.equal(vm.runInContext('numberToLittleEndianHexString(0xfedcba9876543210n)', result.context),
        '1032547698badcfe');
});
console.log(`${cases} JIT protocol tests passed`);
