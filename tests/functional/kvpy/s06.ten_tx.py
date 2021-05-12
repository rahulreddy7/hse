#!/usr/bin/env python3
import hse

import util


hse.init()

try:
    p = hse.Params()
    p.set(key="kvs.transactions_enable", value="1")

    with util.create_kvdb(util.get_kvdb_name(), p) as kvdb:
        with util.create_kvs(kvdb, "ten_tx", p) as kvs:
            cursor_txn = kvdb.transaction()
            cursor_txn.begin()
            holder = kvs.cursor(bind_txn=True, txn=cursor_txn)
            rholder = kvs.cursor(reverse=True, bind_txn=True, txn=cursor_txn)

            txn = kvdb.transaction()
            exp_list = []
            for i in range(1, 11):
                txn.begin()
                k = f"0x00000001000000000000000{i:x}"
                v = f"record{i-1}"
                exp_list.append(v)
                kvs.put(k.encode(), v.encode(), txn=txn)
                kvs.put(b"updateCounter", f"{i}".encode(), txn=txn)
                kvs.put(b"deltaCounter", f"{i}".encode(), txn=txn)
                txn.commit()

            n = len(exp_list)
            exp_list.append(f"{n}")
            exp_list.append(f"{n}")

            assert sum(1 for _ in holder.items()) == 0
            assert sum(1 for _ in rholder.items()) == 0

            cursor_txn.abort()
            cursor_txn = kvdb.transaction()
            cursor_txn.begin()
            holder.update(bind_txn=True, txn=cursor_txn)
            holder.seek(b"0")
            rholder.update(reverse=True, bind_txn=True, txn=cursor_txn)
            rholder.seek(None)

            holder_values = [v.decode() for _, v in holder.items() if v]
            rholder_values = [v.decode() for _, v in rholder.items() if v]
            assert len(holder_values) == len(exp_list)
            for x, y in zip(holder_values, exp_list):
                assert x == y
            for x, y in zip(rholder_values, reversed(exp_list)):
                assert x == y

            holder.seek(b"0x000000010000000000000006")
            _, prev_value = holder.read()
            for _ in range(4):
                _, value = holder.read()
                assert prev_value
                assert value
                assert prev_value < value
                prev_value = value

            rholder.seek(b"0x000000010000000000000006")
            _, prev_value = rholder.read()
            for _ in range(4):
                _, value = rholder.read()
                assert prev_value
                assert value
                assert prev_value > value
                prev_value = value

            holder.destroy()
            rholder.destroy()
finally:
    hse.fini()
