package mmo.activityext.newact20220602.starwheeladventure.model
{
   import mmo.activityext.newact20220602.starwheeladventure.config.SWA_Config;
   import mmo.activityext.newact20220602.starwheeladventure.define.SWA_GoodsDefine;
   import mmo.common.helper.ActUtil;
   import mmo.common.utils.dt.mvc.Model;
   import mmo.interfaces.material.MaterialCommonData;
   
   public class SWA_Model extends Model implements SWA_IModel
   {
      
      private var _todayRestTimes:int;
      
      private var _todayPlayedTimes:int;
      
      private var _weekPlayedTimes:int;
      
      private var _weekRewardState:Boolean;
      
      private var _storeWeekExchangedTimes:int;
      
      private var _storeExchangedData:Object;
      
      private var _progressRewardModel:SWA_ModelForReward = new SWA_ModelForReward();
      
      public function SWA_Model()
      {
         super(SWA_ModelEvent);
      }
      
      public function parseGetStoreInfo(param1:Object) : void
      {
         this._storeWeekExchangedTimes = param1["et"];
         this._storeExchangedData = param1;
      }
      
      public function parseGetGameInfo(param1:Object) : void
      {
         this._todayRestTimes = param1["ti"];
         this._weekPlayedTimes = param1["wgt"];
         this._weekRewardState = param1["wtp"] > 0;
      }
      
      public function parseGetProgressRewardInfo(param1:Object) : void
      {
         this._progressRewardModel.init(param1);
      }
      
      public function get progressRewardModel() : SWA_ModelForReward
      {
         return this._progressRewardModel;
      }
      
      public function afterGetProgressReward(param1:int, param2:Function) : void
      {
         this._progressRewardModel.setRewardState(param1);
         ActUtil.applyCallback(param2);
      }
      
      public function get weekRewardState() : Boolean
      {
         return this._weekRewardState;
      }
      
      public function afterGetWeekReward() : void
      {
         this._weekRewardState = true;
      }
      
      public function setAddPlayedTimes() : void
      {
         --this._todayRestTimes;
         ++this._weekPlayedTimes;
         this._progressRewardModel.addAdventureTimes(1);
      }
      
      public function get todayRestGameTimes() : int
      {
         return this._todayRestTimes;
      }
      
      public function get weekRestGameTimes() : int
      {
         return SWA_Config.WEEKLY_GAME_TIMES - this._weekPlayedTimes;
      }
      
      public function get weekPlayedTimes() : int
      {
         return this._weekPlayedTimes;
      }
      
      public function setAddExchangedGoods(param1:SWA_GoodsDefine, param2:int) : void
      {
         this._storeWeekExchangedTimes += param2;
         this._storeExchangedData[SWA_Config.LIMIT_TYPE_KEY[param1.limitNumType - 1] + param1.id] += param2;
      }
      
      public function get weekRestExcTimes() : int
      {
         return SWA_Config.WEEKLY_EXCHANGE_GOODS_TIMES - this._storeWeekExchangedTimes;
      }
      
      public function getRestExcTimes(param1:SWA_GoodsDefine) : int
      {
         var _loc2_:int = 0;
         if(param1.limitNumType > 0)
         {
            _loc2_ = param1.limitNum;
            return _loc2_ - this._storeExchangedData[SWA_Config.LIMIT_TYPE_KEY[param1.limitNumType - 1] + param1.id];
         }
         return -1;
      }
      
      public function getMyMaterialNum(param1:*) : int
      {
         var _loc2_:MaterialCommonData = null;
         if(param1 is MaterialCommonData)
         {
            _loc2_ = param1;
         }
         else
         {
            _loc2_ = new MaterialCommonData(param1);
         }
         return ActUtil.getMyMaterilaById(_loc2_.type,_loc2_.id);
      }
   }
}

