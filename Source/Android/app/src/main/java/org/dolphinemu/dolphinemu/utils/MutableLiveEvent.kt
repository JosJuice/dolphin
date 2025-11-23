// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

class MutableLiveEvent<T> : LiveEvent<T>() {
    public override fun trigger(t: T) {
        super.trigger(t)
    }
}
