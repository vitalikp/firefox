/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko.fxa.activities;

import android.content.Intent;
import android.os.Bundle;
import org.mozilla.gecko.Locales;
import org.mozilla.gecko.background.common.log.Logger;
import org.mozilla.gecko.fxa.FxAccountConstants;
import org.mozilla.gecko.sync.setup.activities.ActivityUtils;

/**
 * Activity which shows the status activity or passes through to web flow.
 */
public abstract class FxAccountWebFlowActivity extends FxAccountAbstractActivity {
    protected static final String LOG_TAG = FxAccountWebFlowActivity.class.getSimpleName();

    public static final String EXTRA_ENDPOINT = "entrypoint";

    protected static final String[] EXTRAS_TO_PASSTHROUGH = new String[] {
            EXTRA_ENDPOINT,
    };

    private final String action;
    private final String extras;

    public FxAccountWebFlowActivity(int resume, String action) {
        this(resume, action, null);
    }

    public FxAccountWebFlowActivity(int resume, String action, String extras) {
        super(resume);
        this.action = action;
        this.extras = (extras != null) ? ("&" + extras) : "";
    }

    /**
     * {@inheritDoc}
     */
    @Override
    public void onCreate(Bundle icicle) {
        Logger.setThreadLogTag(FxAccountConstants.GLOBAL_LOG_TAG);
        Logger.debug(LOG_TAG, "onCreate(" + icicle + ")");

        Locales.initializeLocale(getApplicationContext());

        super.onCreate(icicle);
    }

    protected boolean redirectIfAppropriate() {
        final boolean redirected = super.redirectIfAppropriate();
        if (redirected) {
            return true;
        }

        return true;
    }

    @Override
    public void onResume() {
        super.onResume();

        // We are always redirected.
        this.finish();
    }
}
