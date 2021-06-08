package org.dolphinemu.dolphinemu.utils;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.net.Uri;
import android.widget.ImageView;

import com.squareup.picasso.Callback;
import com.squareup.picasso.Picasso;

import org.dolphinemu.dolphinemu.R;
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting;
import org.dolphinemu.dolphinemu.model.GameFile;

import java.io.File;

public class PicassoUtils
{
  public static void loadGameBanner(ImageView imageView, GameFile gameFile)
  {
    Picasso picassoInstance = new Picasso.Builder(imageView.getContext())
            .addRequestHandler(new GameBannerRequestHandler(gameFile))
            .build();

    picassoInstance
            .load(Uri.parse("iso:/" + gameFile.getPath()))
            .fit()
            .noFade()
            .noPlaceholder()
            .config(Bitmap.Config.RGB_565)
            .error(R.drawable.no_banner)
            .into(imageView);
  }

  public static void loadGameCover(ImageView imageView, GameFile gameFile)
  {
    if ((new File(gameFile.getCustomCoverPath())).exists())
    {
      loadCustomGameCover(imageView, gameFile);
    }
    else if ((new File(gameFile.getCoverPath())).exists())
    {
      loadCachedGameCover(imageView, gameFile);
    }
    else if (BooleanSetting.MAIN_USE_GAME_COVERS.getBooleanGlobal())
    {
      downloadGameCover(imageView, gameFile);
    }
    else
    {
      loadNoBannerGameCover(imageView, gameFile);
    }
  }

  private static void loadCustomGameCover(ImageView imageView, GameFile gameFile)
  {
    Picasso.get()
            .load(new File(gameFile.getCustomCoverPath()))
            .noFade()
            .noPlaceholder()
            .fit()
            .centerInside()
            .config(Bitmap.Config.ARGB_8888)
            .error(R.drawable.no_banner)
            .into(imageView);
  }

  private static void loadCachedGameCover(ImageView imageView, GameFile gameFile)
  {
    Picasso.get()
            .load(new File(gameFile.getCoverPath()))
            .noFade()
            .noPlaceholder()
            .fit()
            .centerInside()
            .config(Bitmap.Config.ARGB_8888)
            .error(R.drawable.no_banner)
            .into(imageView);
  }

  private static void downloadGameCover(ImageView imageView, GameFile gameFile)
  {
    // GameTDB has a pretty close to complete collection for US/EN covers. First pass at getting
    // the cover will be by the disk's region, second will be the US cover, and third EN.
    downloadGameCover(imageView, gameFile, CoverHelper.getRegion(gameFile),
            PicassoUtils::downloadUsGameCover);
  }

  private static void downloadUsGameCover(ImageView imageView, GameFile gameFile)
  {
    // Second pass using US region
    downloadGameCover(imageView, gameFile, "US", PicassoUtils::downloadEnGameCover);
  }

  private static void downloadEnGameCover(ImageView imageView, GameFile gameFile)
  {
    // Third and last pass using EN region
    downloadGameCover(imageView, gameFile, "EN", PicassoUtils::loadNoBannerGameCover);
  }

  private static void downloadGameCover(ImageView imageView, GameFile gameFile, String region,
          Action2<ImageView, GameFile> onError)
  {
    Picasso.get()
            .load(CoverHelper.buildGameTDBUrl(gameFile, region))
            .noFade()
            .noPlaceholder()
            .fit()
            .centerInside()
            .config(Bitmap.Config.ARGB_8888)
            .error(R.drawable.no_banner)
            .into(imageView, new Callback()
            {
              @Override
              public void onSuccess()
              {
                CoverHelper.saveCover(((BitmapDrawable) imageView.getDrawable()).getBitmap(),
                        gameFile.getCoverPath());
              }

              @Override
              public void onError(Exception ex)
              {
                onError.call(imageView, gameFile);
              }
            });
  }

  private static void loadNoBannerGameCover(ImageView imageView, GameFile gameFile)
  {
    Picasso.get()
          .load(R.drawable.no_banner)
          .noFade()
          .noPlaceholder()
          .fit()
          .centerInside()
          .config(Bitmap.Config.ARGB_8888)
          .into(imageView);
  }
}
