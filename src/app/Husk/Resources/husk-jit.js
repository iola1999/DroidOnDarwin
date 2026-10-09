const CMD_DETACH = 0;
const CMD_PREPARE_REGION = 1;
const CMD_NEW_BREAKPOINTS = 2;
const commands = {
    [CMD_DETACH]: JIT26Detach,
    [CMD_PREPARE_REGION]: JIT26PrepareRegion,
    [CMD_NEW_BREAKPOINTS]: JIT26NewBreakpoints
};
const legacyCommands = {
    [0x68]: JIT26NewBreakpoints,
    [0x69]: JIT26HandleBrk0x69,
    [0xf00d]: JIT26HandleBrk0xf00d
};

let tid, x0, x1, x16, pc;

let detached = false;
let pid = get_pid();
log(`pid = ${pid}`);
let attachResponse = send_command(`vAttach;${pid.toString(16)}`);
log(`attach_response = ${attachResponse}`);
    
let totalBreakpoints = 0;
try {
    if (!/^T[0-9a-f]{2}/i.test(attachResponse)) throw new Error(`Attach failed: ${attachResponse}`);
    while (!detached) {
        totalBreakpoints++;
        log(`Handling breakpoint ${totalBreakpoints}`);

        let brkResponse = send_command(`c`);
        log(`brkResponse = ${brkResponse}`);
        if (!/^T05/.test(brkResponse)) throw new Error(`Unexpected stop: ${brkResponse}`);

        let tmpMatch = /T[0-9a-f]+thread:(?<tid>[0-9a-f]+);/.exec(brkResponse);
        tid = tmpMatch ? tmpMatch.groups['tid'] : null;
        tmpMatch = /20:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
        pc = tmpMatch ? tmpMatch.groups['reg'] : null;
        tmpMatch = /10:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
        x16 = tmpMatch ? tmpMatch.groups['reg'] : null;
        if (!tid || !pc || !x16) {
            throw new Error(`Missing stop registers: ${brkResponse}`);
        }
        pc = littleEndianHexStringToNumber(pc);
        x16 = littleEndianHexStringToNumber(x16);

        let instructionResponse = send_command(`m${pc.toString(16)},4`);
        log(`instruction at pc: ${instructionResponse}`);
        let instrU32 = littleEndianHexToU32(instructionResponse);
        if ((instrU32 & 0xffe0001f) !== (0xd4200000 | 0))
            throw new Error(`Unexpected instruction at 0x${pc.toString(16)}: ${instructionResponse}`);
        let brkImmediate = extractBrkImmediate(instrU32);
        log(`BRK immediate: 0x${brkImmediate.toString(16)} (${brkImmediate})`);
        if (legacyCommands[brkImmediate] != undefined) {
            // when we find a valid brk immediate command, parse x0 and x1
            tmpMatch = /00:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
            x0 = tmpMatch ? tmpMatch.groups['reg'] : null;
            tmpMatch = /01:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
            x1 = tmpMatch ? tmpMatch.groups['reg'] : null;
            if (!x0 || !x1) {
                throw new Error(`Missing argument registers: ${brkResponse}`);
            }
            x0 = littleEndianHexStringToNumber(x0);
            x1 = littleEndianHexStringToNumber(x1);

            // jump over brk
            let pcPlus4 = numberToLittleEndianHexString(pc + 4n);
            let pcPlus4Response = checkedCommand(`P20=${pcPlus4};thread:${tid};`);
            log(`pcPlus4Response = ${pcPlus4Response}`);

            // dispatch brk-immediate command
            const command = legacyCommands[brkImmediate];
            command(brkResponse);
        } else {
            throw new Error(`Unhandled BRK 0x${brkImmediate.toString(16)} at 0x${pc.toString(16)}`);
        }
    }
} catch (error) {
    log(`JIT preparation failed: ${error.message}`);
    if (!detached) JIT26Detach();
    throw error;
}

function checkedCommand(command) {
    const response = send_command(command);
    if (response !== "OK") throw new Error(`${command}: ${response || "empty response"}`);
    return response;
}

function JIT26Detach() {
    let detachResponse = checkedCommand(`D`);
    log(`detachResponse = ${detachResponse}`);
    detached = true;
}

// brk 0x68
function JIT26NewBreakpoints(brkResponse) {
    let instructionResponse = send_command(`m${pc.toString(16)},4`);
    log(`instruction at pc: ${instructionResponse}`);
    let instrU32 = littleEndianHexToU32(instructionResponse);
    let brkImmediate = extractBrkImmediate(instrU32);
    
    let memResponse = send_command(`m${x0.toString(16)},${x1}`);

    let scriptText = hexToAscii(memResponse);
    log(`Script text: ${scriptText}`);

    const res = runScriptAndCapture(scriptText);
    if (res.ok) {
        log('Script succeeded:', res.value);
    } else {
        log('Script failed:', res.name, res.message);
        log(res.stack);
    }
}

// brk 0x69
function JIT26HandleBrk0x69(brkResponse) {
    let putX0Response = checkedCommand(`P0=${numberToLittleEndianHexString(0x690000e0n)};thread:${tid};`);
    log(`putX0Response = ${putX0Response}`);
}

// brk 0xf00d
function JIT26HandleBrk0xf00d(brkResponse) {
    // dispatch command via x16
    const command = commands[x16];
    if (command === undefined) {
        throw new Error(`Unknown command ${x16.toString(16)}`);
    }
    log(`Invoking command ${x16.toString(16)}`);
    command(brkResponse);
}

function JIT26PrepareRegion() {
    try {
        if (x1 === 0n) throw new Error("Empty JIT region");
        let address = x0;
        if (address === 0n) {
            const response = send_command(`_M${x1.toString(16)},rx`);
            if (!/^[0-9a-f]+$/i.test(response) || /^E[0-9a-f]{2}$/i.test(response))
                throw new Error(`RX allocation failed: ${response}`);
            address = BigInt(`0x${response}`);
        }
        const pageSize = 16384n;
        if (address === 0n || address % pageSize !== 0n || x1 % pageSize !== 0n ||
            address + x1 > 0x10000000000000000n)
            throw new Error(`Invalid JIT range: 0x${address.toString(16)} + ${x1}`);

        // StikJIT 1.9.0 formats addresses with nine hex digits and discards page
        // write errors. Send full-width addresses and check each acknowledgement.
        const pages = x1 / pageSize;
        for (let page = 0n; page < pages; ++page) {
            checkedCommand(`M${(address + page * pageSize).toString(16)},1:69`);
            if ((page + 1n) % 256n === 0n || page + 1n === pages)
                log(`JIT region 0x${address.toString(16)}: ${page + 1n}/${pages} pages`);
        }
        checkedCommand(`P0=${numberToLittleEndianHexString(address)};thread:${tid};`);
    } catch (error) {
        // Resume the allocator with failure when the outer handler detaches.
        checkedCommand(`P0=${numberToLittleEndianHexString(0n)};thread:${tid};`);
        throw error;
    }
}

// utilities
function littleEndianHexStringToNumber(hexStr) {
    if (!/^[0-9a-f]{16}$/i.test(hexStr)) throw new Error(`Invalid register: ${hexStr}`);
    return BigInt(`0x${hexStr.match(/../g).reverse().join('')}`);
}

function numberToLittleEndianHexString(num) {
    if (num < 0n || num > 0xffffffffffffffffn) throw new Error(`Register out of range: ${num}`);
    return num.toString(16).padStart(16, '0').match(/../g).reverse().join('');
}

function littleEndianHexToU32(hexStr) {
    if (!/^[0-9a-f]{8}$/i.test(hexStr)) throw new Error(`Invalid instruction: ${hexStr}`);
    return parseInt(hexStr.match(/../g).reverse().join(''), 16);
}

function extractBrkImmediate(u32) {
    return (u32 >> 5) & 0xFFFF;
}

function hexToAscii(hexStr) {
    let str = '';
    for (let i = 0; i < hexStr.length; i += 2) {
        const byte = parseInt(hexStr.substr(i, 2), 16);
        if (byte === 0) break; 
        str += String.fromCharCode(byte);
    }
    return str;
}

function runScriptAndCapture(scriptText) {
    try {
        const value = eval(scriptText);
        return { ok: true, value };
    } catch (err) {
        return {
            ok: false,
            name: err && err.name,
            message: err && err.message,
            stack: err && err.stack
        };
    }
}